# ThemePark Ride Safety & Dispatch Controller
**Final Portfolio Project**  
**Real-Time Systems – Summer 2026**

**Student:** Alfonso Martinez  
**Platform:** ESP32-S3  
**Language:** C  
**Operating System:** FreeRTOS

---

# Wokwi Simulation

https://wokwi.com/projects/470398990653018113

---

# Project Overview

The ThemePark Ride Safety & Dispatch Controller is a real-time embedded application built using the ESP32-S3 and FreeRTOS. The project simulates the control system for a theme park ride where ride sensor data is continuously monitored, processed, and displayed while supporting emergency stop events and runtime fault detection.

The project demonstrates multiple FreeRTOS synchronization primitives working together in a realistic application while separating the real-time control tasks from the monitoring tasks by using both ESP32 cores.

---

# Features

- Queue-based ride sensor pipeline
- Event Groups for synchronization
- Direct Task Notifications
- Binary Semaphore latency comparison
- Mutex protection for shared data
- GPIO Interrupt Service Routine (ISR)
- Heartbeat monitoring
- Watchdog fault detection
- Fault injection mode
- Worst-Case Execution Time (WCET) measurements
- Serial monitoring
- Optional web dashboard

---

# System Architecture

```text
                Core 1 (Real-Time)

        Ride Sensor Task
               │
               ▼
             Queue
               │
               ▼
      Safety Controller
               │
               ▼
          Event Group
               │
               ▼
      Ride Coordinator
               │
     Direct Task Notification
               │
               ▼
     Emergency Responder

Button ISR ───────┘
        │
        └────► Binary Semaphore (Latency Comparison)

               Core 0

      Serial Monitor
      Web Dashboard (optional)
      Heartbeat Watchdog
```
      
## Core 1 (Real-Time Tasks)

- Ride Sensor Task
- Safety Controller Task
- Ride Coordinator Task
- Emergency Responder Task
- Binary Semaphore Latency Task

## Core 0 (Monitoring)

- Serial Monitor
- Web Dashboard (optional)
- Heartbeat Watchdog

Keeping monitoring tasks on Core 0 prevents them from interfering with the timing of the real-time ride control tasks running on Core 1.

---

# FreeRTOS Synchronization Objects

## Queue

The queue transfers ride sensor data from the Ride Sensor task to the Safety Controller task.

Reasons for using a queue:

- Preserves the order of incoming ride data
- Buffers temporary bursts
- Prevents data loss during normal operation

---

## Event Groups

The Event Group synchronizes the ride pipeline.

Event bits include:

- Data Produced
- Data Processed
- Ride Safe
- Emergency Active
- System Fault

The coordinator waits until both producer and consumer finish before continuing the ride cycle.

---

## Direct Task Notification

Direct Task Notifications provide the lowest latency communication between the ISR and the Emergency Responder task.

This is the primary emergency notification path because it is lightweight and optimized for one-to-one task signaling.

---

## Binary Semaphore

The binary semaphore is used only for latency comparison.

The button ISR gives both:

- Direct Task Notification
- Binary Semaphore

This allows the project to compare wake-up latency between the two synchronization methods.

---

## Mutex

A mutex protects shared ride information including:

- Current ride status
- Ride state

Using a mutex prevents race conditions while multiple tasks access the shared data.

---

# Ride State Machine

The controller reports five system states:

- READY
- RUNNING
- WARNING
- EMERGENCY
- FAULT

The Safety Controller updates the current state depending on ride conditions, emergency events, and watchdog status.

---

# Heartbeat Watchdog

Each real-time task increments a heartbeat counter after completing a processing cycle.

The watchdog periodically checks the heartbeats of:

- Producer
- Consumer
- Coordinator
- Responder

If one task stops updating for multiple checks, the watchdog reports a system fault and places the controller into the FAULT state.

---

# Fault Injection

Fault injection is enabled by changing:

```c
#define ENABLE_FAULT_INJECTION 1
```

The Safety Controller intentionally stalls for several seconds to simulate a software failure. This allows the monitoring system to detect abnormal task behavior and demonstrates how failures can be tested without modifying the rest of the application.

---

# Worst-Case Execution Time (WCET)

The project measures the maximum execution time for:

- Producer
- Consumer
- Coordinator
- Responder

These values are displayed in the serial monitor and help evaluate runtime performance.

---

# Emergency Stop

Pressing the GPIO button generates an interrupt.

The ISR immediately:

- Records the interrupt timestamp
- Sends a Direct Task Notification
- Gives the Binary Semaphore
- Returns immediately

The responder measures notification latency while the semaphore task measures semaphore wake latency for comparison.

---

# Performance Summary

| Metric | Typical Value |
|----------|--------------:|
| Producer Period | 50 ms |
| Queue Depth | 8 |
| Notification Latency | ~25–40 µs |
| Binary Semaphore Latency | ~800–1500 µs |
| Producer WCET | ~35 µs |
| Consumer WCET | ~186 µs |
| Coordinator WCET | ~81 µs |
| Responder WCET | ~1300 µs |

During testing, Direct Task Notifications consistently produced lower wake-up latency than Binary Semaphores, making them the preferred choice for ISR-to-task signaling in this application.

---

# Screenshots

## Figure 1 – Normal Operation

<img width="619" height="652" alt="Fig1" src="https://github.com/user-attachments/assets/275a1efc-1b31-4979-91bc-d515618eeac5" />


**Figure 1.** The ThemePark Ride Safety Controller running under normal conditions. The producer, consumer, coordinator, and responder tasks execute continuously while the queue remains empty, heartbeats increase, and the ride transitions between **RUNNING** and **WARNING** based on simulated ride conditions. No emergency or fault conditions are active.

---

## Figure 2 – Emergency Stop

<img width="627" height="557" alt="Fig2" src="https://github.com/user-attachments/assets/23072927-e066-493c-9cc4-cfddef0b7efb" />


**Figure 2.** Pressing the emergency stop button triggers the ISR, which immediately wakes the Emergency Responder through a Direct Task Notification while also measuring Binary Semaphore wake-up latency. The ride transitions into the **EMERGENCY** state.

---

## Figure 3 – Fault Injection

<img width="1791" height="852" alt="Fig3" src="https://github.com/user-attachments/assets/262f906a-50e1-477f-8836-e6c552cbd514" />


**Figure 3.** Fault injection mode intentionally stalls the Safety Controller task for five seconds to simulate a software failure. This demonstrates the fault injection mechanism used to test the system under abnormal operating conditions.

---

# Lessons Learned

This project helped me better understand how different FreeRTOS synchronization primitives work together in a real embedded system. I learned that Direct Task Notifications provide significantly lower latency than Binary Semaphores for one-to-one signaling, while mutexes are better suited for protecting shared resources. Implementing the watchdog and fault injection also demonstrated how embedded systems can detect and respond to failures instead of simply stopping execution.

---

# Future Improvements

Possible improvements include:

- Support multiple rides operating simultaneously
- Store ride events in non-volatile memory
- Add automatic recovery after a fault
- Display historical latency graphs in the web dashboard
- Add additional ride sensors and safety checks

---

# Files Included

- `main.c` – Main application source code
- `diagram.json` – Wokwi hardware configuration
- `README.md` – Project documentation

---

# Running the Project

### Normal Operation

```c
#define USE_WEBSERVER 0
#define ENABLE_FAULT_INJECTION 0
```

### Fault Injection Demonstration

```c
#define ENABLE_FAULT_INJECTION 1
```

### Optional Web Dashboard

```c
#define USE_WEBSERVER 1
```

---

# Conclusion

The ThemePark Ride Safety & Dispatch Controller demonstrates how multiple FreeRTOS synchronization primitives can be combined to build a responsive real-time embedded application. The project integrates queues, event groups, direct task notifications, semaphores, mutexes, interrupts, watchdog monitoring, and runtime performance measurements into a single system. By separating real-time processing from monitoring tasks, the application maintains responsive operation while providing observability, fault detection, and emergency handling.
