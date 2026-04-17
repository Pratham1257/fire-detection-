import pandas as pd
import joblib
from pathlib import Path
from sklearn.model_selection import train_test_split
from sklearn.tree import DecisionTreeClassifier, export_text
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix


FLAME_FIRE_SMOKE_THRESHOLD = 500
DRY_FIRE_HUMIDITY_THRESHOLD = 30
DRY_FIRE_TEMPERATURE_THRESHOLD = 45
DRY_FIRE_SMOKE_THRESHOLD = 700
MILD_SMOKE_MIN = 300
MILD_SMOKE_MAX = 500
SMOKE_WARNING_MAX = 800
GAS_LEAK_MAX = 1200
HIGH_SMOKE_FIRE_TEMPERATURE_THRESHOLD = 50
SENSOR_FAULT_ZERO_VALUE = 0
SENSOR_MALFUNCTION_SMOKE_THRESHOLD = 4000
STEAM_HUMIDITY_THRESHOLD = 85
STEAM_TEMPERATURE_MAX = 50
ELECTRICAL_FIRE_RISK_TEMPERATURE_THRESHOLD = 55
ELECTRICAL_FIRE_RISK_SMOKE_THRESHOLD = 900
ELECTRICAL_FIRE_RISK_HUMIDITY_MAX = 40
HEAT_ALERT_TEMPERATURE_THRESHOLD = 50
MODEL_NAME_PREFIX = "fire_alert_model"
DEFAULT_MODEL_VERSION = "v1"


def get_model_path(version=DEFAULT_MODEL_VERSION):
    return f"{MODEL_NAME_PREFIX}_{version}.pkl"


def build_sample_weights(X_train):
    sample_weights = pd.Series(1.0, index=X_train.index)

    high_fire_risk = (
        (X_train["flame_detected"] == 1)
        & (X_train["smoke_gas_level"] > FLAME_FIRE_SMOKE_THRESHOLD)
    )
    sample_weights.loc[high_fire_risk] = 3.0

    dry_hot_smoky_risk = (
        (X_train["humidity"] < DRY_FIRE_HUMIDITY_THRESHOLD)
        & (X_train["temperature"] > DRY_FIRE_TEMPERATURE_THRESHOLD)
        & (X_train["smoke_gas_level"] > DRY_FIRE_SMOKE_THRESHOLD)
    )
    sample_weights.loc[dry_hot_smoky_risk] = 2.5

    return sample_weights


def load_data(csv_path="fire_detection_dataset.csv"):
    data = pd.read_csv(csv_path)
    X = data[["temperature", "humidity", "smoke_gas_level", "flame_detected"]]
    y = data["label"]
    return X, y


def train_model(X, y, test_size=0.2, random_state=42, max_depth=5):
    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=test_size, random_state=random_state, stratify=y
    )

    # Prioritize high-risk fire patterns during training.
    sample_weights = build_sample_weights(X_train)

    model = DecisionTreeClassifier(max_depth=max_depth, random_state=random_state)
    model.fit(X_train, y_train, sample_weight=sample_weights)
    return model, X_test, y_test


def evaluate_model(model, X_test, y_test):
    y_pred = model.predict(X_test)
    acc = accuracy_score(y_test, y_pred)
    report = classification_report(y_test, y_pred)
    labels = list(model.classes_)
    matrix = confusion_matrix(y_test, y_pred, labels=labels)
    matrix_df = pd.DataFrame(matrix, index=labels, columns=labels)
    return acc, report, matrix_df


def compare_depths(X, y, depths=(5, 7, 10), test_size=0.2, random_state=42):
    X_train, X_test, y_train, y_test = train_test_split(
        X, y, test_size=test_size, random_state=random_state, stratify=y
    )
    sample_weights = build_sample_weights(X_train)

    comparison_rows = []
    for depth in depths:
        model = DecisionTreeClassifier(max_depth=depth, random_state=random_state)
        model.fit(X_train, y_train, sample_weight=sample_weights)

        train_acc = accuracy_score(y_train, model.predict(X_train))
        test_acc = accuracy_score(y_test, model.predict(X_test))
        gap = train_acc - test_acc

        comparison_rows.append(
            {
                "max_depth": depth,
                "train_accuracy": train_acc,
                "test_accuracy": test_acc,
                "overfit_gap": gap,
            }
        )

    return pd.DataFrame(comparison_rows)


def save_model(model, path=None, version=DEFAULT_MODEL_VERSION):
    target_path = path or get_model_path(version)
    joblib.dump(model, target_path)
    return target_path


def load_model(path=None, version=DEFAULT_MODEL_VERSION):
    if path:
        return joblib.load(path)

    versioned_path = get_model_path(version)
    if Path(versioned_path).exists():
        return joblib.load(versioned_path)

    # Backward compatibility with the old single-file model name.
    legacy_path = "fire_alert_model.pkl"
    return joblib.load(legacy_path)


def rule_based_alert(temperature, humidity, smoke_gas_level, flame_detected):
    if (
        temperature == SENSOR_FAULT_ZERO_VALUE
        and humidity == SENSOR_FAULT_ZERO_VALUE
        and smoke_gas_level == SENSOR_FAULT_ZERO_VALUE
        and flame_detected == 0
    ):
        return "Sensor Fault"

    if smoke_gas_level > SENSOR_MALFUNCTION_SMOKE_THRESHOLD:
        return "Sensor Malfunction"

    # Safety-first override: stable flame detection is treated as an immediate emergency.
    if flame_detected == 1:
        return "Immediate Fire Alert"

    if (
        humidity < DRY_FIRE_HUMIDITY_THRESHOLD
        and temperature > DRY_FIRE_TEMPERATURE_THRESHOLD
        and smoke_gas_level > DRY_FIRE_SMOKE_THRESHOLD
    ):
        return "Immediate Fire Alert"

    if (
        smoke_gas_level > GAS_LEAK_MAX
        and temperature > HIGH_SMOKE_FIRE_TEMPERATURE_THRESHOLD
    ):
        return "Immediate Fire Alert"

    if (
        flame_detected == 0
        and temperature > ELECTRICAL_FIRE_RISK_TEMPERATURE_THRESHOLD
        and smoke_gas_level > ELECTRICAL_FIRE_RISK_SMOKE_THRESHOLD
        and humidity < ELECTRICAL_FIRE_RISK_HUMIDITY_MAX
    ):
        return "Electrical Fire Risk"

    if (
        flame_detected == 0
        and humidity > STEAM_HUMIDITY_THRESHOLD
        and MILD_SMOKE_MIN <= smoke_gas_level < SMOKE_WARNING_MAX
        and temperature < STEAM_TEMPERATURE_MAX
    ):
        return "Steam Detected"

    if (
        flame_detected == 0
        and temperature > HEAT_ALERT_TEMPERATURE_THRESHOLD
        and smoke_gas_level < MILD_SMOKE_MIN
    ):
        return "Heat Alert"

    if MILD_SMOKE_MIN <= smoke_gas_level < MILD_SMOKE_MAX:
        return "Mild Smoke Alert"

    if MILD_SMOKE_MAX <= smoke_gas_level < SMOKE_WARNING_MAX:
        return "Smoke Warning Alert"

    if SMOKE_WARNING_MAX <= smoke_gas_level <= GAS_LEAK_MAX:
        return "Gas Leakage Alert"

    return "No Immediate Alert"


def predict_event(
    model,
    temperature,
    humidity,
    smoke_gas_level,
    flame_detected,
    with_confidence=False,
):
    if (
        temperature == SENSOR_FAULT_ZERO_VALUE
        and humidity == SENSOR_FAULT_ZERO_VALUE
        and smoke_gas_level == SENSOR_FAULT_ZERO_VALUE
        and flame_detected == 0
    ):
        if with_confidence:
            return "Sensor Fault", 0.99
        return "Sensor Fault"

    if smoke_gas_level > SENSOR_MALFUNCTION_SMOKE_THRESHOLD:
        if with_confidence:
            return "Sensor Malfunction", 0.99
        return "Sensor Malfunction"

    # Safety-first override: any flame trigger is escalated as Fire Emergency.
    if flame_detected == 1:
        if with_confidence:
            return "Fire Emergency", 1.0
        return "Fire Emergency"

    if (
        humidity < DRY_FIRE_HUMIDITY_THRESHOLD
        and temperature > DRY_FIRE_TEMPERATURE_THRESHOLD
        and smoke_gas_level > DRY_FIRE_SMOKE_THRESHOLD
    ):
        if with_confidence:
            return "Fire Emergency", 1.0
        return "Fire Emergency"

    if (
        smoke_gas_level > GAS_LEAK_MAX
        and temperature > HIGH_SMOKE_FIRE_TEMPERATURE_THRESHOLD
    ):
        if with_confidence:
            return "Fire Emergency", 1.0
        return "Fire Emergency"

    if (
        flame_detected == 0
        and temperature > ELECTRICAL_FIRE_RISK_TEMPERATURE_THRESHOLD
        and smoke_gas_level > ELECTRICAL_FIRE_RISK_SMOKE_THRESHOLD
        and humidity < ELECTRICAL_FIRE_RISK_HUMIDITY_MAX
    ):
        if with_confidence:
            return "Electrical Fire Risk", 0.95
        return "Electrical Fire Risk"

    if (
        flame_detected == 0
        and humidity > STEAM_HUMIDITY_THRESHOLD
        and MILD_SMOKE_MIN <= smoke_gas_level < SMOKE_WARNING_MAX
        and temperature < STEAM_TEMPERATURE_MAX
    ):
        if with_confidence:
            return "Steam Detected", 0.92
        return "Steam Detected"

    if (
        flame_detected == 0
        and temperature > HEAT_ALERT_TEMPERATURE_THRESHOLD
        and smoke_gas_level < MILD_SMOKE_MIN
    ):
        if with_confidence:
            return "Heat Alert", 0.90
        return "Heat Alert"

    input_data = pd.DataFrame([{
        "temperature": temperature,
        "humidity": humidity,
        "smoke_gas_level": smoke_gas_level,
        "flame_detected": flame_detected
    }])

    probabilities = model.predict_proba(input_data)[0]
    prediction = model.predict(input_data)[0]
    class_index = list(model.classes_).index(prediction)
    confidence = float(probabilities[class_index])

    if with_confidence:
        return prediction, confidence
    return prediction


def export_rules(model, path="esp32_decision_rules.txt"):
    feature_names = ["temperature", "humidity", "smoke_gas_level", "flame_detected"]
    rules = export_text(model, feature_names=feature_names)
    with open(path, "w", encoding="utf-8") as rule_file:
        rule_file.write("Decision Tree Rules for ESP32 Deployment\n")
        rule_file.write("=======================================\n\n")
        rule_file.write("Priority Safety Rule (evaluate first):\n")
        rule_file.write(
            f"IF flame_detected == 1 AND smoke_gas_level > {FLAME_FIRE_SMOKE_THRESHOLD}: class = Fire Emergency\n\n"
        )
        rule_file.write("Priority Dry-Heat Rule (evaluate first):\n")
        rule_file.write(
            "IF humidity < "
            f"{DRY_FIRE_HUMIDITY_THRESHOLD} "
            "AND temperature > "
            f"{DRY_FIRE_TEMPERATURE_THRESHOLD} "
            "AND smoke_gas_level > "
            f"{DRY_FIRE_SMOKE_THRESHOLD}: class = Fire Emergency\n\n"
        )
        rule_file.write("Early-Warning Progression:\n")
        rule_file.write(
            f"IF {MILD_SMOKE_MIN} <= smoke_gas_level < {MILD_SMOKE_MAX}: Mild Smoke Alert\n"
        )
        rule_file.write(
            f"IF {MILD_SMOKE_MAX} <= smoke_gas_level < {SMOKE_WARNING_MAX}: Smoke Warning Alert\n"
        )
        rule_file.write(
            f"IF {SMOKE_WARNING_MAX} <= smoke_gas_level <= {GAS_LEAK_MAX}: Gas Leakage Alert\n"
        )
        rule_file.write(
            "IF smoke_gas_level > "
            f"{GAS_LEAK_MAX} "
            "AND temperature > "
            f"{HIGH_SMOKE_FIRE_TEMPERATURE_THRESHOLD}: Immediate Fire Alert\n\n"
        )
        rule_file.write("Industrial Context Labels:\n")
        rule_file.write(
            "IF temperature == 0 AND humidity == 0 AND smoke_gas_level == 0: Sensor Fault\n"
        )
        rule_file.write(
            "IF smoke_gas_level > "
            f"{SENSOR_MALFUNCTION_SMOKE_THRESHOLD}: Sensor Malfunction\n"
        )
        rule_file.write(
            "IF flame_detected == 0 AND humidity > "
            f"{STEAM_HUMIDITY_THRESHOLD} "
            "AND 300 <= smoke_gas_level < 800 AND temperature < "
            f"{STEAM_TEMPERATURE_MAX}: Steam Detected\n"
        )
        rule_file.write(
            "IF flame_detected == 0 AND temperature > "
            f"{HEAT_ALERT_TEMPERATURE_THRESHOLD} "
            "AND smoke_gas_level < 300: Heat Alert\n"
        )
        rule_file.write(
            "IF flame_detected == 0 AND temperature > "
            f"{ELECTRICAL_FIRE_RISK_TEMPERATURE_THRESHOLD} "
            "AND smoke_gas_level > "
            f"{ELECTRICAL_FIRE_RISK_SMOKE_THRESHOLD} "
            "AND humidity < "
            f"{ELECTRICAL_FIRE_RISK_HUMIDITY_MAX}: Electrical Fire Risk\n\n"
        )
        rule_file.write(rules)
    return rules


def classify_alert(prediction):
    outcome = {
        "No Fire": {
            "verification": "No fire condition confirmed",
            "emergency_level": "Low",
            "action": "Continue monitoring sensor stream.",
            "authority": []
        },
        "Fire": {
            "verification": "Real emergency confirmed",
            "emergency_level": "Critical",
            "action": "Trigger alarm, evacuate, and notify emergency teams.",
            "authority": ["Fire station", "Hospital", "Police"]
        },
        "False Alarm": {
            "verification": "False alarm confirmed",
            "emergency_level": "Low",
            "action": "Monitor only, no evacuation needed.",
            "authority": []
        },
        "Smoke Warning": {
            "verification": "Real alert detected",
            "emergency_level": "Medium",
            "action": "Check smoke source and stay alert.",
            "authority": ["Fire response standby", "Building security"]
        },
        "Heat Alert": {
            "verification": "Abnormal heat condition detected",
            "emergency_level": "Medium",
            "action": "Inspect heat source and increase ventilation.",
            "authority": ["Building security", "Maintenance team"]
        },
        "Steam Detected": {
            "verification": "Likely steam/moisture interference detected",
            "emergency_level": "Low",
            "action": "Check for steam source and verify with camera/manual inspection.",
            "authority": []
        },
        "Gas Leakage": {
            "verification": "Real alert detected",
            "emergency_level": "High",
            "action": "Ventilate area, avoid sparks, inspect gas source.",
            "authority": ["Gas safety team", "Emergency support"]
        },
        "Electrical Fire Risk": {
            "verification": "Potential electrical ignition risk detected",
            "emergency_level": "High",
            "action": "Cut power to affected zone and dispatch electrical safety inspection.",
            "authority": ["Electrical safety team", "Fire response standby"]
        },
        "Sensor Fault": {
            "verification": "Sensor reliability issue detected",
            "emergency_level": "Medium",
            "action": "Run sensor diagnostics and maintenance before trusting readings.",
            "authority": ["Maintenance team"]
        },
        "Sensor Malfunction": {
            "verification": "Sensor output exceeds trusted range",
            "emergency_level": "Medium",
            "action": "Isolate faulty sensor channel and perform calibration or replacement.",
            "authority": ["Maintenance team"]
        },
        "Fire Emergency": {
            "verification": "Real emergency confirmed",
            "emergency_level": "Critical",
            "action": "Trigger alarm, evacuate, begin emergency response.",
            "authority": ["Fire station", "Hospital", "Police"]
        }
    }
    return outcome.get(prediction, {
        "verification": "Unknown",
        "emergency_level": "Unknown",
        "action": "Check sensors and run diagnostics.",
        "authority": []
    })


if __name__ == "__main__":
    X, y = load_data()

    depth_comparison = compare_depths(X, y, depths=(5, 7, 10))
    print("Depth Comparison (train vs test):")
    print(depth_comparison.to_string(index=False, float_format=lambda v: f"{v:.6f}"))
    print()

    model, X_test, y_test = train_model(X, y)
    accuracy, report, confusion_df = evaluate_model(model, X_test, y_test)
    print("Accuracy:", accuracy)
    print(report)
    print("Confusion Matrix (rows=actual, cols=predicted):")
    print(confusion_df.to_string())
    saved_model_path = save_model(model, version=DEFAULT_MODEL_VERSION)
    export_rules(model)
    print(f"Model saved as {saved_model_path}")
    print("Rules exported as esp32_decision_rules.txt")
