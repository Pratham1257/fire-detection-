from fire_detection import load_model, predict_event, classify_alert, rule_based_alert


def run_tests():
    errors = []

    if rule_based_alert(25.0, 60.0, 100, 0) != "No Immediate Alert":
        errors.append("rule_based_alert false alarm case failed")

    if rule_based_alert(35.0, 45.0, 350, 0) != "Mild Smoke Alert":
        errors.append("rule_based_alert mild smoke case failed")

    if rule_based_alert(35.0, 45.0, 500, 0) != "Smoke Warning Alert":
        errors.append("rule_based_alert smoke warning case failed")

    if rule_based_alert(35.0, 45.0, 900, 0) != "Gas Leakage Alert":
        errors.append("rule_based_alert gas leakage band case failed")

    if rule_based_alert(33.0, 55.0, 550, 1) != "Immediate Fire Alert":
        errors.append("rule_based_alert flame priority case failed")

    if rule_based_alert(48.0, 25.0, 780, 0) != "Immediate Fire Alert":
        errors.append("rule_based_alert dry heat humidity case failed")

    if rule_based_alert(55.0, 35.0, 1300, 0) != "Immediate Fire Alert":
        errors.append("rule_based_alert high smoke high temperature case failed")

    if rule_based_alert(58.0, 35.0, 1000, 0) != "Electrical Fire Risk":
        errors.append("rule_based_alert electrical fire risk case failed")

    if rule_based_alert(40.0, 90.0, 450, 0) != "Steam Detected":
        errors.append("rule_based_alert steam detected case failed")

    if rule_based_alert(55.0, 35.0, 220, 0) != "Heat Alert":
        errors.append("rule_based_alert heat alert case failed")

    if rule_based_alert(0.0, 0.0, 0, 0) != "Sensor Fault":
        errors.append("rule_based_alert sensor fault case failed")

    if rule_based_alert(30.0, 40.0, 4500, 0) != "Sensor Malfunction":
        errors.append("rule_based_alert sensor malfunction case failed")

    model = load_model()

    prediction_false = predict_event(model, 28, 60, 180, 0)
    if prediction_false != "False Alarm":
        errors.append(f"model false alarm case failed, got {prediction_false}")

    prediction_smoke = predict_event(model, 38, 48, 500, 0)
    if prediction_smoke != "Smoke Warning":
        errors.append(f"model smoke warning case failed, got {prediction_smoke}")

    prediction_smoke_with_conf, confidence_smoke = predict_event(
        model, 38, 48, 500, 0, with_confidence=True
    )
    if prediction_smoke_with_conf != "Smoke Warning":
        errors.append(
            "model confidence output changed class label unexpectedly"
        )
    if not (0.0 <= confidence_smoke <= 1.0):
        errors.append(f"model confidence out of range, got {confidence_smoke}")

    prediction_gas = predict_event(model, 34, 40, 860, 0)
    if prediction_gas != "Gas Leakage":
        errors.append(f"model gas leakage case failed, got {prediction_gas}")

    prediction_flame_priority = predict_event(model, 33, 55, 550, 1)
    if prediction_flame_priority != "Fire Emergency":
        errors.append(
            f"model flame priority case failed, got {prediction_flame_priority}"
        )

    prediction_flame_priority_conf, confidence_flame_priority = predict_event(
        model, 33, 55, 550, 1, with_confidence=True
    )
    if prediction_flame_priority_conf != "Fire Emergency":
        errors.append(
            "model flame priority confidence output changed class label unexpectedly"
        )
    if confidence_flame_priority < 0.99:
        errors.append(
            f"model flame priority confidence too low, got {confidence_flame_priority}"
        )

    prediction_dry_heat = predict_event(model, 48, 25, 780, 0)
    if prediction_dry_heat != "Fire Emergency":
        errors.append(f"model dry heat humidity case failed, got {prediction_dry_heat}")

    prediction_high_smoke_temp = predict_event(model, 55, 35, 1300, 0)
    if prediction_high_smoke_temp != "Fire Emergency":
        errors.append(
            f"model high smoke high temperature case failed, got {prediction_high_smoke_temp}"
        )

    prediction_electrical = predict_event(model, 58, 35, 1000, 0)
    if prediction_electrical != "Electrical Fire Risk":
        errors.append(f"model electrical fire risk case failed, got {prediction_electrical}")

    prediction_steam = predict_event(model, 40, 90, 450, 0)
    if prediction_steam != "Steam Detected":
        errors.append(f"model steam detected case failed, got {prediction_steam}")

    prediction_heat = predict_event(model, 55, 35, 220, 0)
    if prediction_heat != "Heat Alert":
        errors.append(f"model heat alert case failed, got {prediction_heat}")

    prediction_fault, confidence_fault = predict_event(
        model, 0, 0, 0, 0, with_confidence=True
    )
    if prediction_fault != "Sensor Fault":
        errors.append(f"model sensor fault case failed, got {prediction_fault}")
    if confidence_fault < 0.95:
        errors.append(f"model sensor fault confidence too low, got {confidence_fault}")

    prediction_malfunction, confidence_malfunction = predict_event(
        model, 30, 40, 4500, 0, with_confidence=True
    )
    if prediction_malfunction != "Sensor Malfunction":
        errors.append(
            f"model sensor malfunction case failed, got {prediction_malfunction}"
        )
    if confidence_malfunction < 0.95:
        errors.append(
            f"model sensor malfunction confidence too low, got {confidence_malfunction}"
        )

    prediction_fire = predict_event(model, 64, 20, 1500, 1)
    result = classify_alert(prediction_fire)
    if prediction_fire != "Fire Emergency":
        errors.append(f"model fire emergency case failed, got {prediction_fire}")
    if result["emergency_level"] != "Critical":
        errors.append("classification emergency level is not Critical")
    if "Fire station" not in result["authority"]:
        errors.append("classification authority does not include Fire station")

    electrical_result = classify_alert("Electrical Fire Risk")
    if electrical_result["emergency_level"] != "High":
        errors.append("electrical fire risk emergency level is not High")

    steam_result = classify_alert("Steam Detected")
    if steam_result["emergency_level"] != "Low":
        errors.append("steam detected emergency level is not Low")

    malfunction_result = classify_alert("Sensor Malfunction")
    if malfunction_result["emergency_level"] != "Medium":
        errors.append("sensor malfunction emergency level is not Medium")

    if errors:
        print("TEST FAILED")
        for e in errors:
            print(" -", e)
        return 1

    print("All tests passed")
    return 0


if __name__ == "__main__":
    exit(run_tests())