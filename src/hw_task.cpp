#include "hw_task.h"
#include <Arduino.h>

struct Job {
  const std::function<void()>* fn;
  SemaphoreHandle_t done;
};

static QueueHandle_t s_queue = nullptr;
static TaskHandle_t s_loopTask = nullptr;

void hwBegin() {
  s_queue = xQueueCreate(4, sizeof(Job));
  s_loopTask = xTaskGetCurrentTaskHandle();
}

void hwPoll() {
  Job job;
  while (xQueueReceive(s_queue, &job, 0) == pdTRUE) {
    (*job.fn)();
    xSemaphoreGive(job.done);
  }
}

void hwRun(const std::function<void()>& fn) {
  if (xTaskGetCurrentTaskHandle() == s_loopTask) {
    fn();
    return;
  }
  Job job{&fn, xSemaphoreCreateBinary()};
  xQueueSend(s_queue, &job, portMAX_DELAY);
  xSemaphoreTake(job.done, portMAX_DELAY);
  vSemaphoreDelete(job.done);
}
