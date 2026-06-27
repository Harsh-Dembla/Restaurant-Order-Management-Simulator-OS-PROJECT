# Restaurant-Order-Management-Simulator-OS-PROJECT
A C restaurant management simulator that manages orders, tables, menu items, billing, and customer records using object-oriented programming.
# Restaurant Order Management Simulator
## OS Concepts Demo — C + Web Dashboard

---

## What it demonstrates

| OS Concept              | Implementation                                                    |
|-------------------------|-------------------------------------------------------------------|
| Producer-Consumer       | Waiter thread produces orders; Chef threads consume them          |
| Mutex locks             | `pthread_mutex_t` protects shared queue and order array           |
| Semaphores              | `sem_t kitchen_sem` limits concurrent chefs (kitchen capacity)    |
| Condition variables     | `pthread_cond_t` for queue-full / queue-empty blocking            |
| Priority queue          | Max-heap: VIP orders always dequeued before normal orders         |
| Dynamic allocation      | Allocator thread adds/removes chefs based on queue depth          |
| Shared resource mgmt    | Single `all_orders[]` array, accessed safely under mutex          |

---

## File structure

```
restaurant_sim/
├── restaurant_sim.c   ← C backend (compile & run this)
├── api.php            ← PHP bridge (optional — for live mode)
├── index.html         ← Web dashboard (works standalone in demo mode)
└── README.md
```

---

## Quick start

### Option A — Demo mode (no C needed)
Just open `index.html` in a browser. The JavaScript engine simulates the restaurant in real-time.

---

### Option B — Live mode with C backend

#### Step 1 — Compile

```bash
gcc -o restaurant_sim restaurant_sim.c -lpthread -lm -O2
```

On macOS the semaphore API differs; use a POSIX-compliant Linux environment or WSL on Windows.

#### Step 2 — Run the C simulator

```bash
./restaurant_sim
```

It writes `sim_state.json` every 0.5 seconds.

#### Step 3 — Serve the web files with PHP

```bash
# In the same directory as index.html and api.php:
php -S localhost:8080
```

Then open: http://localhost:8080

The dashboard will auto-detect the live backend and switch out of demo mode.

---

### Option C — Live mode with Python HTTP server (read-only, no cancel/speed)

```bash
python3 -m http.server 8080
```

This serves `index.html` and `sim_state.json` but cannot handle POST commands (cancel/speed). The dashboard will show live order data but cancel and speed controls will silently fail.

---

## Controls

| Control          | What it does                                              |
|------------------|-----------------------------------------------------------|
| Speed selector   | 0.5x → 5x — scales waiter interval and prep times        |
| Cancel button    | Marks an order cancelled (removed from queue or prep)     |

---

## Architecture diagram

```
┌─────────────────────────────────────────────────────┐
│                  C Simulation Process                 │
│                                                       │
│  ┌──────────────┐         ┌─────────────────────┐    │
│  │ Waiter Thread│──push──▶│   Priority Queue     │    │
│  │  (Producer)  │         │  (mutex-protected)   │    │
│  └──────────────┘         └──────────┬──────────┘    │
│                                      │ pop (VIP first)│
│  ┌──────────────────────────────┐    │                │
│  │  Chef Threads  (Consumers)   │◀───┘                │
│  │  semaphore limits capacity   │                     │
│  │  Chef1 Chef2 … Chef8         │                     │
│  └──────────────────────────────┘                     │
│                                                       │
│  ┌────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  Allocator │  │Cancel Watcher│  │Speed Watcher  │  │
│  │  Thread    │  │(cancel_cmd)  │  │(speed_cmd)    │  │
│  └────────────┘  └──────────────┘  └──────────────┘  │
│                                                       │
│  ┌────────────────────────────────────────────────┐   │
│  │  State Writer → sim_state.json (every 500ms)   │   │
│  └────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────┘
              ▲                        ▲
              │ PHP api.php            │ periodic AJAX poll
              │ (POST: cancel/speed)   │ (GET: status)
              ▼                        ▼
┌────────────────────────────────────────────────┐
│          index.html  (Browser)                  │
│  Kanban board — Chefs panel — Activity log      │
│  Live stats — Cancel buttons — Speed control    │
└────────────────────────────────────────────────┘
```

---

## Key C threading details

### Mutex usage
```c
pthread_mutex_lock(&queue_mutex);
// ... safely push/pop from priority queue ...
pthread_mutex_unlock(&queue_mutex);
```

### Semaphore for kitchen capacity
```c
sem_init(&kitchen_sem, 0, active_chefs);  // N permits

// Chef thread acquires before processing:
sem_timedwait(&kitchen_sem, &ts);         // blocks if kitchen full
// ... process order ...
sem_post(&kitchen_sem);                   // release
```

### Condition variable for producer-consumer coordination
```c
// Waiter blocks when queue is full:
while (queue.size >= MAX_QUEUE && running)
    pthread_cond_wait(&queue_not_full, &queue_mutex);

// Chef wakes waiter after popping:
pthread_cond_signal(&queue_not_full);

// Chef blocks when queue empty:
pthread_cond_timedwait(&queue_not_empty, &queue_mutex, &tw);

// Waiter wakes chef after pushing:
pthread_cond_signal(&queue_not_empty);
```

### Priority queue (max-heap)
```c
// VIP orders (priority=1) get a score 1,000,000 higher than normal
static int pq_priority(const Order *o) {
    return o->priority * 1000000 + age_in_seconds;
}
// pq_push: bubble up; pq_pop: remove root, sift down
```

---

## Requirements

- Linux / macOS / WSL
- GCC with pthreads: `gcc -lpthread -lm`
- PHP 7.4+ for live mode (optional)
- Any modern browser for the dashboard

- ------------------------Live dashboard of restuarant-simulator---------------------
- <img width="954" height="441" alt="image" src="https://github.com/user-attachments/assets/8110a979-8173-48c7-95a6-26a7b4be62be" />


---
