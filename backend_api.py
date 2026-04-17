from datetime import datetime, timedelta
import logging
import os
from typing import Optional

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

from database import (
    init_db,
    insert_event,
    insert_notifications,
    find_recent_duplicate_event,
    get_recent_events,
    get_dashboard_stats,
)
from fire_detection import load_model, predict_event, rule_based_alert, classify_alert

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s | %(levelname)s | %(name)s | %(message)s",
)
logger = logging.getLogger("macro_project_backend")

HIGH_SMOKE_WARNING_LEVEL = 800
MAX_ACCEPTED_TEMPERATURE = 100
MIN_REALISTIC_HUMIDITY = 5
DUPLICATE_WINDOW_SECONDS = 30
SMOKE_BUCKET_SIZE = 100

app = FastAPI(title="Macro Project Backend API", version="1.0.0")
model = None
model_load_error = None


class SensorPayload(BaseModel):
    device_id: Optional[str] = None
    temperature: float = Field(ge=0, le=120)
    humidity: float = Field(ge=0, le=100)
    smoke_gas_level: float = Field(ge=0, le=5000)
    flame_detected: int = Field(ge=0, le=1)


def calculate_severity_score(payload: SensorPayload):
    severity_score = 0

    if payload.smoke_gas_level > 500:
        severity_score += 30

    if payload.temperature > 45:
        severity_score += 30

    if payload.flame_detected == 1:
        severity_score += 40

    if severity_score < 0:
        return 0
    if severity_score > 100:
        return 100
    return severity_score


def get_severity_band(severity_score):
    if severity_score <= 20:
        return "Safe"
    if severity_score <= 50:
        return "Warning"
    if severity_score <= 80:
        return "High Risk"
    return "Critical"


def validate_sensor_values(payload: SensorPayload):
    issues = []

    if payload.temperature < 0 or payload.temperature > MAX_ACCEPTED_TEMPERATURE:
        issues.append(
            f"temperature must be between 0 and {MAX_ACCEPTED_TEMPERATURE}"
        )

    if payload.smoke_gas_level < 0:
        issues.append("smoke_gas_level cannot be negative")

    if (
        payload.humidity < MIN_REALISTIC_HUMIDITY
        and payload.smoke_gas_level == 0
        and payload.temperature != 0
        and payload.flame_detected == 0
    ):
        issues.append(
            "humidity below 5 with smoke_gas_level 0 and no flame may indicate sensor fault"
        )

    return issues


@app.on_event("startup")
def startup_event():
    global model, model_load_error
    model_version = os.getenv("MODEL_VERSION", "v1")

    try:
        init_db()
        logger.info("Database initialized successfully")
    except Exception as exc:
        logger.error("Database connection failed: %s", exc)
        raise

    try:
        model = load_model(version=model_version)
        model_load_error = None
        logger.info("Model loaded successfully (version=%s)", model_version)
    except Exception as exc:
        model = None
        model_load_error = str(exc)
        logger.error("Model loading failed (version=%s): %s", model_version, exc)
        logger.warning("API will continue in degraded mode until a valid model is available")


@app.get("/health")
def health_check():
    if model is None:
        return {
            "status": "degraded",
            "service": "macro-project-backend",
            "model_loaded": False,
            "model_error": model_load_error,
        }

    return {
        "status": "ok",
        "service": "macro-project-backend",
        "model_loaded": True,
    }


@app.post("/api/v1/events/predict")
def predict_and_store(payload: SensorPayload):
    if model is None:
        logger.error("Prediction attempted before model initialization")
        raise HTTPException(
            status_code=503,
            detail=f"Model is not initialized: {model_load_error or 'load failure'}",
        )

    validation_issues = validate_sensor_values(payload)
    if validation_issues:
        logger.warning(
            "Sensor validation failed: device=%s issues=%s",
            payload.device_id,
            "; ".join(validation_issues),
        )
        raise HTTPException(
            status_code=422,
            detail={
                "message": "Invalid or unrealistic sensor values",
                "issues": validation_issues,
            },
        )

    if payload.smoke_gas_level >= HIGH_SMOKE_WARNING_LEVEL:
        logger.warning(
            "High smoke level detected: device=%s smoke=%s temp=%s humidity=%s flame=%s",
            payload.device_id,
            payload.smoke_gas_level,
            payload.temperature,
            payload.humidity,
            payload.flame_detected,
        )

    try:
        initial_warning = rule_based_alert(
            payload.temperature,
            payload.humidity,
            payload.smoke_gas_level,
            payload.flame_detected,
        )
        prediction, prediction_confidence = predict_event(
            model,
            payload.temperature,
            payload.humidity,
            payload.smoke_gas_level,
            payload.flame_detected,
            with_confidence=True,
        )
        severity_score = calculate_severity_score(payload)
        severity_band = get_severity_band(severity_score)
        result = classify_alert(prediction)
        now = datetime.now()
        created_at = now.strftime("%Y-%m-%d %H:%M:%S")
        duplicate_cutoff = (now - timedelta(seconds=DUPLICATE_WINDOW_SECONDS)).strftime(
            "%Y-%m-%d %H:%M:%S"
        )

        duplicate_event = find_recent_duplicate_event(
            payload.device_id,
            prediction,
            payload.smoke_gas_level,
            duplicate_cutoff,
            smoke_bucket_size=SMOKE_BUCKET_SIZE,
        )
        duplicate_suppressed = duplicate_event is not None
        if duplicate_suppressed:
            logger.info(
                "Duplicate pattern detected: device=%s previous_id=%s prediction=%s. Event will be stored but notification suppressed.",
                payload.device_id,
                duplicate_event["id"],
                prediction,
            )

        authority_list = result["authority"]
        notify_required = len(authority_list) > 0 and not duplicate_suppressed

        event = {
            "created_at": created_at,
            "device_id": payload.device_id,
            "temperature": payload.temperature,
            "humidity": payload.humidity,
            "smoke_gas_level": payload.smoke_gas_level,
            "flame_detected": payload.flame_detected,
            "initial_warning": initial_warning,
            "prediction": prediction,
            "prediction_confidence": prediction_confidence,
            "prediction_display": f"{prediction} ({prediction_confidence * 100:.0f}%)",
            "severity_score": severity_score,
            "severity_band": severity_band,
            "verification": result["verification"],
            "emergency_level": result["emergency_level"],
            "action": result["action"],
            "authority": ", ".join(authority_list) if authority_list else "None",
            "notify_required": notify_required,
        }

        event_id = insert_event(event)
        if notify_required:
            insert_notifications(event_id, authority_list, created_at)

        logger.info(
            "Event stored: id=%s device=%s prediction=%s confidence=%.3f",
            event_id,
            payload.device_id,
            prediction,
            prediction_confidence,
        )

        return {
            "event_id": event_id,
            "event": event,
            "authorities": authority_list,
            "notification_status": (
                "queued"
                if notify_required
                else ("skipped_duplicate_notification" if duplicate_suppressed else "skipped_false_alarm")
            ),
            "duplicate_skipped": duplicate_suppressed,
        }
    except HTTPException:
        raise
    except Exception as exc:
        logger.error("Prediction pipeline failed: %s", exc)
        raise HTTPException(status_code=500, detail="Prediction pipeline failed")


@app.get("/api/v1/events")
def list_events(limit: int = 200):
    return {"events": get_recent_events(limit)}


@app.get("/api/v1/stats")
def stats():
    return get_dashboard_stats()
