/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#define SRC_MAIN_SCHEDULER_C_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "build/build_config.h"
#include "build/debug.h"
#include "common/maths.h"
#include "common/time.h"
#include "common/utils.h"
#include "config/config_unittest.h"
#include "drivers/time.h"
#include "platform.h"
#include "scheduler/scheduler.h"

// DEBUG_SCHEDULER, timings for:
// 0 - gyroUpdate()
// 1 - pidController()
// 2 - time spent in scheduler
// 3 - time spent executing check function

static FAST_RAM_ZERO_INIT cfTask_t *currentTask = NULL;

static FAST_RAM_ZERO_INIT uint32_t totalWaitingTasks;
static FAST_RAM_ZERO_INIT uint32_t totalWaitingTasksSamples;

static FAST_RAM_ZERO_INIT bool calculateTaskStatistics;
FAST_RAM_ZERO_INIT uint16_t averageSystemLoadPercent = 0;

static FAST_RAM_ZERO_INIT int taskQueuePos = 0;
STATIC_UNIT_TESTED FAST_RAM_ZERO_INIT int taskQueueSize = 0;

static FAST_RAM int periodCalculationBasisOffset =
  offsetof(cfTask_t, lastExecutedAt);

// No need for a linked list for the queue, since items are only inserted at
// startup

STATIC_UNIT_TESTED FAST_RAM_ZERO_INIT
  cfTask_t *taskQueueArray[TASK_COUNT +
                           1];  // extra item for NULL pointer at end of queue

void queueClear(void) {
    memset(taskQueueArray, 0, sizeof(taskQueueArray));
    taskQueuePos = 0;
    taskQueueSize = 0;
}

bool queueContains(cfTask_t *task) {
    for (int ii = 0; ii < taskQueueSize; ++ii) {
        if (taskQueueArray[ii] == task) {
            return true;
        }
    }
    return false;
}

bool queueAdd(cfTask_t *task) {
    if ((taskQueueSize >= TASK_COUNT) || queueContains(task)) {
        return false;
    }
    for (int ii = 0; ii <= taskQueueSize; ++ii) {
        if (taskQueueArray[ii] == NULL ||
            taskQueueArray[ii]->staticPriority < task->staticPriority) {
            memmove(&taskQueueArray[ii + 1],
                    &taskQueueArray[ii],
                    sizeof(task) * (taskQueueSize - ii));
            taskQueueArray[ii] = task;
            ++taskQueueSize;
            return true;
        }
    }
    return false;
}

bool queueRemove(cfTask_t *task) {
    for (int ii = 0; ii < taskQueueSize; ++ii) {
        if (taskQueueArray[ii] == task) {
            memmove(&taskQueueArray[ii],
                    &taskQueueArray[ii + 1],
                    sizeof(task) * (taskQueueSize - ii));
            --taskQueueSize;
            return true;
        }
    }
    return false;
}

/*
 * Returns first item queue or NULL if queue empty
 */
FAST_CODE cfTask_t *queueFirst(void) {
    taskQueuePos = 0;
    return taskQueueArray[0];  // guaranteed to be NULL if queue is empty
}

/*
 * Returns next item in queue or NULL if at end of queue
 */
FAST_CODE cfTask_t *queueNext(void) {
    return taskQueueArray[++taskQueuePos];  // guaranteed to be NULL at end of
                                            // queue
}

void taskSystemLoad(timeUs_t currentTimeUs) {
    UNUSED(currentTimeUs);

    // Calculate system load
    if (totalWaitingTasksSamples > 0) {
        averageSystemLoadPercent =
          100 * totalWaitingTasks / totalWaitingTasksSamples;
        totalWaitingTasksSamples = 0;
        totalWaitingTasks = 0;
    }
#if defined(SIMULATOR_BUILD)
    averageSystemLoadPercent = 0;
#endif
}

#if defined(USE_TASK_STATISTICS)
#define MOVING_SUM_COUNT 32
timeUs_t checkFuncMaxExecutionTime;
timeUs_t checkFuncTotalExecutionTime;
timeUs_t checkFuncMovingSumExecutionTime;
timeUs_t checkFuncMovingSumDeltaTime;

void getCheckFuncInfo(cfCheckFuncInfo_t *checkFuncInfo) {
    checkFuncInfo->maxExecutionTime = checkFuncMaxExecutionTime;
    checkFuncInfo->totalExecutionTime = checkFuncTotalExecutionTime;
    checkFuncInfo->averageExecutionTime =
      checkFuncMovingSumExecutionTime / MOVING_SUM_COUNT;
    checkFuncInfo->averageDeltaTime =
      checkFuncMovingSumDeltaTime / MOVING_SUM_COUNT;
}
#endif

void getTaskInfo(cfTaskId_e taskId, cfTaskInfo_t *taskInfo) {
    taskInfo->isEnabled = queueContains(&cfTasks[taskId]);
    taskInfo->desiredPeriod = cfTasks[taskId].desiredPeriod;
    taskInfo->staticPriority = cfTasks[taskId].staticPriority;
#if defined(USE_TASK_STATISTICS)
    taskInfo->taskName = cfTasks[taskId].taskName;
    taskInfo->subTaskName = cfTasks[taskId].subTaskName;
    taskInfo->maxExecutionTime = cfTasks[taskId].maxExecutionTime;
    taskInfo->totalExecutionTime = cfTasks[taskId].totalExecutionTime;
    taskInfo->averageExecutionTime =
      cfTasks[taskId].movingSumExecutionTime / MOVING_SUM_COUNT;
    taskInfo->averageDeltaTime =
      cfTasks[taskId].movingSumDeltaTime / MOVING_SUM_COUNT;
    taskInfo->latestDeltaTime = cfTasks[taskId].taskLatestDeltaTime;
    taskInfo->movingAverageCycleTime = cfTasks[taskId].movingAverageCycleTime;
#endif
}

void rescheduleTask(cfTaskId_e taskId, uint32_t newPeriodMicros) {
    if (taskId == TASK_SELF) {
        cfTask_t *task = currentTask;
        task->desiredPeriod =
          MAX(SCHEDULER_DELAY_LIMIT,
              (timeDelta_t)newPeriodMicros);  // Limit delay to 100us (10 kHz)
                                              // to prevent scheduler clogging
    } else if (taskId < TASK_COUNT) {
        cfTask_t *task = &cfTasks[taskId];
        task->desiredPeriod =
          MAX(SCHEDULER_DELAY_LIMIT,
              (timeDelta_t)newPeriodMicros);  // Limit delay to 100us (10 kHz)
                                              // to prevent scheduler clogging
    }
}

void setTaskEnabled(cfTaskId_e taskId, bool enabled) {
    if (taskId == TASK_SELF || taskId < TASK_COUNT) {
        cfTask_t *task = taskId == TASK_SELF ? currentTask : &cfTasks[taskId];
        if (enabled && task->taskFunc) {
            queueAdd(task);
        } else {
            queueRemove(task);
        }
    }
}

timeDelta_t getTaskDeltaTime(cfTaskId_e taskId) {
    if (taskId == TASK_SELF) {
        return currentTask->taskLatestDeltaTime;
    } else if (taskId < TASK_COUNT) {
        return cfTasks[taskId].taskLatestDeltaTime;
    } else {
        return 0;
    }
}

void schedulerSetCalulateTaskStatistics(bool calculateTaskStatisticsToUse) {
    calculateTaskStatistics = calculateTaskStatisticsToUse;
}

void schedulerResetTaskStatistics(cfTaskId_e taskId) {
#if defined(USE_TASK_STATISTICS)
    if (taskId == TASK_SELF) {
        currentTask->movingSumExecutionTime = 0;
        currentTask->movingSumDeltaTime = 0;
        currentTask->totalExecutionTime = 0;
        currentTask->maxExecutionTime = 0;
    } else if (taskId < TASK_COUNT) {
        cfTasks[taskId].movingSumExecutionTime = 0;
        cfTasks[taskId].movingSumDeltaTime = 0;
        cfTasks[taskId].totalExecutionTime = 0;
        cfTasks[taskId].maxExecutionTime = 0;
    }
#else
    UNUSED(taskId);
#endif
}

void schedulerResetTaskMaxExecutionTime(cfTaskId_e taskId) {
#if defined(USE_TASK_STATISTICS)
    if (taskId == TASK_SELF) {
        currentTask->maxExecutionTime = 0;
    } else if (taskId < TASK_COUNT) {
        cfTasks[taskId].maxExecutionTime = 0;
    }
#else
    UNUSED(taskId);
#endif
}

void schedulerInit(void) {
    calculateTaskStatistics = true;
    queueClear();
    queueAdd(&cfTasks[TASK_SYSTEM]);
}

void schedulerOptimizeRate(bool optimizeRate) {
    periodCalculationBasisOffset = optimizeRate
                                     ? offsetof(cfTask_t, lastDesiredAt)
                                     : offsetof(cfTask_t, lastExecutedAt);
}

inline static timeUs_t getPeriodCalculationBasis(const cfTask_t *task) {
    if (task->staticPriority == TASK_PRIORITY_REALTIME) {
        return *(timeUs_t *)((uint8_t *)task + periodCalculationBasisOffset);
    } else {
        return task->lastExecutedAt;
    }
}

FAST_CODE void scheduler(void) {
    // Cache currentTime

    const timeUs_t currentTimeUs = micros();

    // Check for realtime tasks
    bool outsideRealtimeGuardInterval = true;
    for (const cfTask_t *task = queueFirst();
         task != NULL && task->staticPriority >= TASK_PRIORITY_REALTIME;
         task = queueNext()) {
        const timeUs_t nextExecuteAt =
          getPeriodCalculationBasis(task) + task->desiredPeriod;
        if ((timeDelta_t)(currentTimeUs - nextExecuteAt) >= 0) {
            outsideRealtimeGuardInterval = false;

            break;
        }
    }

    // The task to be invoked
    cfTask_t *selectedTask = NULL;
    uint16_t selectedTaskDynamicPriority = 0;

    // Update task dynamic priorities
    uint16_t waitingTasks = 0;
    for (cfTask_t *task = queueFirst(); task != NULL; task = queueNext()) {
        // Task has checkFunc - event driven
        if (task->checkFunc) {
#if defined(SCHEDULER_DEBUG)
            const timeUs_t currentTimeBeforeCheckFuncCall = micros();
#else
            const timeUs_t currentTimeBeforeCheckFuncCall = currentTimeUs;
#endif
            // Increase priority for event driven tasks
            if (task->dynamicPriority > 0) {
                task->taskAgeCycles =
                  1 + ((currentTimeUs - task->lastSignaledAt) /
                       task->desiredPeriod);
                task->dynamicPriority =
                  1 + task->staticPriority * task->taskAgeCycles;
                waitingTasks++;
            } else if (task->checkFunc(currentTimeBeforeCheckFuncCall,
                                       currentTimeBeforeCheckFuncCall -
                                         task->lastExecutedAt)) {
#if defined(SCHEDULER_DEBUG)
                DEBUG_SET(DEBUG_SCHEDULER,
                          3,
                          micros() - currentTimeBeforeCheckFuncCall);
#endif
#if defined(USE_TASK_STATISTICS)
                if (calculateTaskStatistics) {
                    const uint32_t checkFuncExecutionTime =
                      micros() - currentTimeBeforeCheckFuncCall;
                    checkFuncMovingSumExecutionTime +=
                      checkFuncExecutionTime -
                      checkFuncMovingSumExecutionTime / MOVING_SUM_COUNT;
                    checkFuncMovingSumDeltaTime +=
                      task->taskLatestDeltaTime -
                      checkFuncMovingSumDeltaTime / MOVING_SUM_COUNT;
                    checkFuncTotalExecutionTime +=
                      checkFuncExecutionTime;  // time consumed by scheduler +
                                               // task
                    checkFuncMaxExecutionTime =
                      MAX(checkFuncMaxExecutionTime, checkFuncExecutionTime);
                }
#endif
                task->lastSignaledAt = currentTimeBeforeCheckFuncCall;
                task->taskAgeCycles = 1;
                task->dynamicPriority = 1 + task->staticPriority;
                waitingTasks++;
            } else {
                task->taskAgeCycles = 0;
            }
        } else {
            // Task is time-driven, dynamicPriority is last execution age
            // (measured in desiredPeriods) Task age is calculated from last
            // execution
            task->taskAgeCycles =
              ((currentTimeUs - getPeriodCalculationBasis(task)) /
               task->desiredPeriod);
            if (task->taskAgeCycles > 0) {
                task->dynamicPriority =
                  1 + task->staticPriority * task->taskAgeCycles;
                waitingTasks++;
            }
        }

        if (task->dynamicPriority > selectedTaskDynamicPriority) {
            const bool taskCanBeChosenForScheduling =
              (outsideRealtimeGuardInterval) || (task->taskAgeCycles > 1) ||
              (task->staticPriority == TASK_PRIORITY_REALTIME);
            if (taskCanBeChosenForScheduling) {
                selectedTaskDynamicPriority = task->dynamicPriority;
                selectedTask = task;
            }
        }
    }

    totalWaitingTasksSamples++;
    totalWaitingTasks += waitingTasks;

    currentTask = selectedTask;

    if (selectedTask) {
        // Found a task that should be run
        selectedTask->taskLatestDeltaTime =
          currentTimeUs - selectedTask->lastExecutedAt;
        float period = currentTimeUs - selectedTask->lastExecutedAt;
        selectedTask->lastExecutedAt = currentTimeUs;
        selectedTask->lastDesiredAt +=
          (cmpTimeUs(currentTimeUs, selectedTask->lastDesiredAt) /
           selectedTask->desiredPeriod) *
          selectedTask->desiredPeriod;
        selectedTask->dynamicPriority = 0;

        // Execute task
#if defined(USE_TASK_STATISTICS)
        if (calculateTaskStatistics) {
            const timeUs_t currentTimeBeforeTaskCall = micros();
            selectedTask->taskFunc(currentTimeBeforeTaskCall);
            const timeUs_t taskExecutionTime =
              micros() - currentTimeBeforeTaskCall;
            selectedTask->movingSumExecutionTime +=
              taskExecutionTime -
              selectedTask->movingSumExecutionTime / MOVING_SUM_COUNT;
            selectedTask->movingSumDeltaTime +=
              selectedTask->taskLatestDeltaTime -
              selectedTask->movingSumDeltaTime / MOVING_SUM_COUNT;
            selectedTask->totalExecutionTime +=
              taskExecutionTime;  // time consumed by scheduler + task
            selectedTask->maxExecutionTime =
              MAX(selectedTask->maxExecutionTime, taskExecutionTime);
            selectedTask->movingAverageCycleTime +=
              0.05f * (period - selectedTask->movingAverageCycleTime);
        } else
#endif
        {
            selectedTask->taskFunc(currentTimeUs);
        }

#if defined(SCHEDULER_DEBUG)
        DEBUG_SET(DEBUG_SCHEDULER,
                  2,
                  micros() - currentTimeUs -
                    taskExecutionTime);  // time spent in scheduler
    } else {
        DEBUG_SET(DEBUG_SCHEDULER, 2, micros() - currentTimeUs);
#endif
    }

    GET_SCHEDULER_LOCALS();
}
