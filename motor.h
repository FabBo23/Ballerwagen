#pragma once

// ========================= Interrupt Service Routinen =========================

void IRAM_ATTR buttonM_ISR() {
    unsigned long t = millis();
    if (t - lastButtonMTime > DEBOUNCE_DELAY_MS) {
        buttonMPressedFlag = true;
        lastButtonMTime    = t;
        if (!mButtonWasPressedAndHeld && digitalRead(BUTTON_M_PIN) == LOW) {
            mButtonPressStartTime      = t;
            mButtonWasPressedAndHeld   = true;
            mButtonLongPressActionDone = false;
        }
    }
}

void IRAM_ATTR buttonDown_ISR() {
    unsigned long t = millis();
    if (t - lastButtonDownTime > DEBOUNCE_DELAY_MS) {
        lastButtonDownTime = t;
        drehzahlSollwert   = max(drehzahlSollwert - drehzahlSchrittweite, 0);
    }
}

void IRAM_ATTR buttonUp_ISR() {
    unsigned long t = millis();
    if (t - lastButtonUpTime > DEBOUNCE_DELAY_MS) {
        lastButtonUpTime = t;
        drehzahlSollwert = min(drehzahlSollwert + drehzahlSchrittweite, 100);
    }
}

void IRAM_ATTR hornButton_ISR() {
    unsigned long t = millis();
    if (t - lastHornButtonTime > HORN_BUTTON_DEBOUNCE_MS) {
        lastHornButtonTime       = t;
        hornButtonPressStartTime = t;
        hornButtonPhysicalHeld   = true;
        digitalWrite(RELAY_CH4_PIN, HIGH);
        hornStopTime = t + hornShortPressDurationMs;
    }
}

void IRAM_ATTR hallSensor_ISR() {
    unsigned long t = millis();
    if (t - lastPulseTime > HALL_DEBOUNCE_DELAY_MS) {
        PulseTime              = t - lastPulseTime;
        lastPulseTime          = t;
        hallSensorPulseDetected = true;
    }
}

// ========================= Pin & Interrupt Setup =========================

void setupPins() {
    pinMode(BUTTON_M_PIN,    INPUT_PULLUP);
    pinMode(BUTTON_DOWN_PIN, INPUT_PULLUP);
    pinMode(BUTTON_UP_PIN,   INPUT_PULLUP);
    pinMode(IN4_PIN,         INPUT_PULLUP);
    pinMode(HORN_BUTTON_PIN, INPUT_PULLUP);
    pinMode(HALL_SENSOR_PIN, INPUT_PULLUP);

    pinMode(RELAY_FREIGABE_PIN,    OUTPUT);
    pinMode(RELAY_RUECKWAERTS_PIN, OUTPUT);
    pinMode(RELAY_CH3_PIN,         OUTPUT);
    pinMode(RELAY_CH4_PIN,         OUTPUT);
    digitalWrite(RELAY_FREIGABE_PIN,    LOW);
    digitalWrite(RELAY_RUECKWAERTS_PIN, LOW);
    digitalWrite(RELAY_CH3_PIN,         LOW);
    digitalWrite(RELAY_CH4_PIN,         LOW);
    dacWrite(DAC_PIN_VO1, 0);
}

void setupInterrupts() {
    attachInterrupt(digitalPinToInterrupt(BUTTON_M_PIN),    buttonM_ISR,    FALLING);
    attachInterrupt(digitalPinToInterrupt(BUTTON_DOWN_PIN), buttonDown_ISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(BUTTON_UP_PIN),   buttonUp_ISR,   FALLING);
    attachInterrupt(digitalPinToInterrupt(HORN_BUTTON_PIN), hornButton_ISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(HALL_SENSOR_PIN), hallSensor_ISR, FALLING);
}

// ========================= Motorsteuerung =========================

void setPotProzent(int percentage) {
    percentage = constrain(percentage, 0, 100);
    dacWrite(DAC_PIN_VO1, map(percentage, 0, 100, DAC_MIN_OUTPUT, DAC_MAX_OUTPUT));
}

void sanftAnlauf() {
    unsigned long t = millis();
    if (drehzahlIstwert != drehzahlSollwert
        && t - lastStepTime >= (unsigned long)rampDelay
        && deadmanSwitchActive) {
        lastStepTime = t;
        if (drehzahlIstwert < drehzahlSollwert) drehzahlIstwert++;
        else                                     drehzahlIstwert--;
        setPotProzent(drehzahlIstwert);
    }
    digitalWrite(RELAY_FREIGABE_PIN,
                 (deadmanSwitchActive && drehzahlIstwert > 0) ? HIGH : LOW);
}

void manageMButton() {
    if (!mButtonWasPressedAndHeld) return;
    if (digitalRead(BUTTON_M_PIN) == LOW) {
        if (!mButtonLongPressActionDone
            && (millis() - mButtonPressStartTime >= mButtonLongPressDurationMs)) {
            // Richtung wechseln
            drehrichtungF = !drehrichtungF;
            drehrichtungR = !drehrichtungR;
            drehzahlSollwert = 0;
            digitalWrite(RELAY_RUECKWAERTS_PIN, drehrichtungR ? HIGH : LOW);
            DBG_PRINT("M-Taste lang: Richtung → ");
            DBG_PRINTLN(drehrichtungF ? "Vorwärts" : "Rückwärts");
            mButtonLongPressActionDone = true;
        }
    } else {
        mButtonWasPressedAndHeld = false;
    }
}

// ========================= Hupensteuerung =========================

void manageHornLogic() {
    if (hornButtonPhysicalHeld) {
        if (digitalRead(HORN_BUTTON_PIN) == LOW) {
            if (millis() - hornButtonPressStartTime < hornMaxPressDurationMs) {
                digitalWrite(RELAY_CH4_PIN, HIGH);
                hornStopTime = millis() + 50;
            } else {
                digitalWrite(RELAY_CH4_PIN, LOW);
                hornStopTime = 0;
                hornButtonPhysicalHeld = false;
            }
        } else {
            hornButtonPhysicalHeld = false;
        }
    }
    if (hornStopTime > 0 && millis() >= hornStopTime) {
        digitalWrite(RELAY_CH4_PIN, LOW);
        hornStopTime           = 0;
        hornButtonPhysicalHeld = false;
    }
}

// ========================= Totmannschalter =========================

void checkDeadmanSwitch() {
    if (digitalRead(IN4_PIN) == HIGH) {
        if (deadmanSwitchActive) {
            DBG_PRINTLN("Totmannschalter UNTERBROCHEN");
            deadmanSwitchActive = false;
            buttonMPressedFlag  = false;
            digitalWrite(RELAY_CH3_PIN, LOW);
        }
    } else {
        if (!deadmanSwitchActive && buttonMPressedFlag) {
            DBG_PRINTLN("Totmannschalter AKTIV");
            deadmanSwitchActive = true;
            buttonMPressedFlag  = false;
        }
        digitalWrite(RELAY_CH3_PIN, HIGH);
    }
    if (!deadmanSwitchActive) {
        drehzahlIstwert    = 0;
        buttonMPressedFlag = false;
    }
}

// ========================= Geschwindigkeitsmessung =========================

void calculateAndUpdateSpeed() {
    if (millis() - lastSpeedCalcTime < SPEED_CALC_INTERVAL_MS) return;

    noInterrupts();
    unsigned long pt          = PulseTime;
    unsigned long lastPulse   = lastPulseTime;
    lastSpeedCalcTime         = millis();
    interrupts();

    if (pt >= HALL_DEBOUNCE_DELAY_MS && pt <= SPEED_CALC_INTERVAL_MS
        && PULSES_PER_REVOLUTION > 0 && WHEEL_CIRCUMFERENCE_M > 0.0f) {
        float rps       = 1.0f / (pt / 1000.0f) / (float)PULSES_PER_REVOLUTION;
        currentSpeedKmh = constrain(rps * WHEEL_CIRCUMFERENCE_M * 3.6f, 0.0f, 6.8f);
    }
    if (millis() - lastPulse > SPEED_CALC_INTERVAL_MS + 500) {
        currentSpeedKmh = 0.0f;
    }
}
