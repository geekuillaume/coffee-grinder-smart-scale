#include "scale.hpp"
#include <MathBuffer.h>


HX711 loadcell;
SimpleKalmanFilter kalmanFilter(0.2, 0.2, 0.05);


#define ABS(a) (((a) > 0) ? (a) : ((a) * -1))

TaskHandle_t ScaleTask;
TaskHandle_t ScaleStatusTask;

double scaleWeight = 0;
unsigned long scaleLastUpdatedAt = 0;
unsigned long lastSignificantWeightChangeAt = 0;
unsigned long lastTareAt = 0; // if 0, should tare load cell, else represent when it was last tared
bool scaleReady = false;
int scaleStatus = STATUS_EMPTY;
double cupWeightEmpty = 0;
unsigned long startedGrindingAt = 0;
unsigned long finishedGrindingAt = 0;
MathBuffer<double, 100> weightHistory;


void tareScale() {
  Serial.println("Taring scale");
  loadcell.tare(TARE_MEASURES);
  lastTareAt = millis();
}

void updateScale( void * parameter) {
  float lastEstimate;


  for (;;) {
    if (lastTareAt == 0) {
      tareScale();
    }
    if (loadcell.wait_ready_timeout(300)) {
      lastEstimate = kalmanFilter.updateEstimate(loadcell.get_units());
      scaleWeight = lastEstimate;
      scaleLastUpdatedAt = millis();
      weightHistory.push(scaleWeight);
      scaleReady = true;
    } else {
      Serial.println("HX711 not found.");
      scaleReady = false;
    }
  }
}

void scaleStatusLoop(void *p) {
  double tenSecAvg;
  for (;;) {
    tenSecAvg = weightHistory.averageSince((int64_t)millis() - 10000);
    // Serial.printf("Avg: %f, currentWeight: %f\n", tenSecAvg, scaleWeight);

    if (ABS(tenSecAvg - scaleWeight) > SIGNIFICANT_WEIGHT_CHANGE) {
      // Serial.printf("Detected significant change: %f\n", ABS(avg - scaleWeight));
      lastSignificantWeightChangeAt = millis();
    }


    if (scaleStatus == STATUS_EMPTY) {
      if (millis() - lastTareAt > TARE_MIN_INTERVAL && ABS(tenSecAvg) > 0.2 && tenSecAvg < 3 && scaleWeight < 3) {
        // tare if: not tared recently, more than 0.2 away from 0, less than 3 grams total (also works for negative weight)
        lastTareAt = 0;
      }

      if (ABS(weightHistory.minSince((int64_t)millis() - 1000) - CUP_WEIGHT) < CUP_DETECTION_TOLERANCE &&
        ABS(weightHistory.maxSince((int64_t)millis() - 1000) - CUP_WEIGHT) < CUP_DETECTION_TOLERANCE
      ) {
        // using average over last 500ms as empty cup weight
        Serial.println("Starting grinding");
        cupWeightEmpty = weightHistory.averageSince((int64_t)millis() - 500);
        scaleStatus = STATUS_GRINDING_IN_PROGRESS;
        startedGrindingAt = millis();
        digitalWrite(GRINDER_ACTIVE_PIN, 1);
        continue;
      }
    } else if (scaleStatus == STATUS_GRINDING_IN_PROGRESS) {
      if (!scaleReady) {
        digitalWrite(GRINDER_ACTIVE_PIN, 0);
        scaleStatus = STATUS_GRINDING_FAILED;
      }

      if (millis() - startedGrindingAt > MAX_GRINDING_TIME) {
        Serial.println("Failed because grinding took too long");
        digitalWrite(GRINDER_ACTIVE_PIN, 0);
        scaleStatus = STATUS_GRINDING_FAILED;
        continue;
      }

      if (
        millis() - startedGrindingAt > 2000 && // started grinding at least 2s ago
        scaleWeight - weightHistory.firstValueOlderThan(millis() - 2000) < 1 // less than a gram has been grinded in the last 2 second
      ) {
        Serial.println("Failed because no change in weight was detected");
        digitalWrite(GRINDER_ACTIVE_PIN, 0);
        scaleStatus = STATUS_GRINDING_FAILED;
        continue;
      }

      if (weightHistory.minSince((int64_t)millis() - 200) < cupWeightEmpty - CUP_DETECTION_TOLERANCE) {
        Serial.printf("Failed because weight too low, min: %f, min value: %f\n", weightHistory.minSince((int64_t)millis() - 200), CUP_WEIGHT + CUP_DETECTION_TOLERANCE);
        digitalWrite(GRINDER_ACTIVE_PIN, 0);
        scaleStatus = STATUS_GRINDING_FAILED;
        continue;
      }
      if (weightHistory.maxSince((int64_t)millis() - 200) >= cupWeightEmpty + COFFEE_DOSE_WEIGHT) {
        Serial.println("Finished grinding");
        finishedGrindingAt = millis();
        digitalWrite(GRINDER_ACTIVE_PIN, 0);
        scaleStatus = STATUS_GRINDING_FINISHED;
        continue;
      }
    } else if (scaleStatus == STATUS_GRINDING_FINISHED) {
      if (scaleWeight < 5) {
        Serial.println("Going back to empty");
        scaleStatus = STATUS_EMPTY;
        continue;
      }
    } else if (scaleStatus == STATUS_GRINDING_FAILED) {
      if (scaleWeight >= GRINDING_FAILED_WEIGHT_TO_RESET) {
        Serial.println("Going back to empty");
        scaleStatus = STATUS_EMPTY;
        continue;
      }
    }

    delay(50);
  }
}

void setupScale() {
  loadcell.begin(LOADCELL_DOUT_PIN, LOADCELL_SCK_PIN);
  loadcell.set_scale(LOADCELL_SCALE_FACTOR);

  pinMode(GRINDER_ACTIVE_PIN, OUTPUT);
  digitalWrite(GRINDER_ACTIVE_PIN, 0);

  xTaskCreatePinnedToCore(
      updateScale, /* Function to implement the task */
      "Scale", /* Name of the task */
      10000,  /* Stack size in words */
      NULL,  /* Task input parameter */
      0,  /* Priority of the task */
      &ScaleTask,  /* Task handle. */
      1); /* Core where the task should run */


  xTaskCreatePinnedToCore(
      scaleStatusLoop, /* Function to implement the task */
      "ScaleStatus", /* Name of the task */
      10000,  /* Stack size in words */
      NULL,  /* Task input parameter */
      0,  /* Priority of the task */
      &ScaleStatusTask,  /* Task handle. */
      1); /* Core where the task should run */
}
