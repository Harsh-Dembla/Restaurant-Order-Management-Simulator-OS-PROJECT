
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <signal.h>


#define MAX_QUEUE       50
#define MAX_ORDERS      500
#define MAX_CHEFS       8
#define MIN_CHEFS       2
#define MAX_LOG_LINES   200
#define STATE_FILE      "sim_state.json"
#define CANCEL_FILE     "cancel_cmd.txt"
#define SPEED_FILE      "speed_cmd.txt"


typedef enum {
    STATE_RECEIVED    = 0,
    STATE_IN_QUEUE    = 1,
    STATE_PREPARING   = 2,
    STATE_COMPLETED   = 3,
    STATE_CANCELLED   = 4
} OrderState;


typedef struct {
    int   id;
    int   priority;      /* 0 = normal, 1 = VIP */
    char  item[64];
    int   prep_time;     /* seconds */
    OrderState state;
    time_t created_at;
    time_t completed_at;
    int   chef_id;       /* which chef is handling it */
    int   cancelled;
} Order;

typedef struct {
    Order *data[MAX_QUEUE];
    int   size;
} PriorityQueue;


typedef struct {
    int       id;
    int       active;
    int       orders_done;
    Order    *current_order;
    pthread_t thread;
} Chef;

static PriorityQueue  queue;
static Order          all_orders[MAX_ORDERS];
static int            order_count    = 0;
static int            next_order_id  = 1;
static int            running        = 1;
static int            active_chefs   = MIN_CHEFS;
static double         sim_speed      = 1.0;   /* 1.0 = normal, 2.0 = 2x faster */

static pthread_mutex_t queue_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_mutex   = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex     = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  queue_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  queue_not_full  = PTHREAD_COND_INITIALIZER;
static sem_t           kitchen_sem;   /* limits concurrent chefs */

static Chef   chefs[MAX_CHEFS];
static char   log_lines[MAX_LOG_LINES][256];
static int    log_head = 0, log_count = 0;

static const char *menu_items[] = {
    "Margherita Pizza", "Grilled Salmon", "Caesar Salad",
    "Beef Burger",      "Pasta Carbonara","Chicken Tikka",
    "Sushi Platter",    "Lamb Chops",     "Vegetable Stir-fry",
    "Chocolate Lava Cake"
};
static const int menu_times[] = { 12, 15, 5, 8, 10, 14, 18, 20, 7, 9 };
#define MENU_SIZE 10


static void log_event(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char entry[256];
    snprintf(entry, sizeof(entry), "[%02d:%02d:%02d] %s",
             t->tm_hour, t->tm_min, t->tm_sec, buf);

    pthread_mutex_lock(&log_mutex);
    int idx = (log_head + log_count) % MAX_LOG_LINES;
    strncpy(log_lines[idx], entry, 255);
    if (log_count < MAX_LOG_LINES) log_count++;
    else log_head = (log_head + 1) % MAX_LOG_LINES;
    pthread_mutex_unlock(&log_mutex);
}


static int pq_priority(const Order *o) {

    return o->priority * 1000000 + (int)(time(NULL) - o->created_at);
}

static void pq_push(PriorityQueue *pq, Order *o) {
    if (pq->size >= MAX_QUEUE) return;
    int i = pq->size++;
    pq->data[i] = o;
    /* bubble up */
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (pq_priority(pq->data[i]) > pq_priority(pq->data[parent])) {
            Order *tmp = pq->data[i];
            pq->data[i] = pq->data[parent];
            pq->data[parent] = tmp;
            i = parent;
        } else break;
    }
}

static Order *pq_pop(PriorityQueue *pq) {
    if (pq->size == 0) return NULL;
    Order *top = pq->data[0];
    pq->data[0] = pq->data[--pq->size];
    /* bubble down */
    int i = 0;
    while (1) {
        int l = 2*i+1, r = 2*i+2, best = i;
        if (l < pq->size && pq_priority(pq->data[l]) > pq_priority(pq->data[best])) best = l;
        if (r < pq->size && pq_priority(pq->data[r]) > pq_priority(pq->data[best])) best = r;
        if (best == i) break;
        Order *tmp = pq->data[i];
        pq->data[i] = pq->data[best];
        pq->data[best] = tmp;
        i = best;
    }
    return top;
}

static void *chef_thread(void *arg) {
    Chef *chef = (Chef *)arg;
    log_event("Chef %d entered the kitchen", chef->id);

    while (running) {
       
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        if (sem_timedwait(&kitchen_sem, &ts) != 0) continue;

        pthread_mutex_lock(&queue_mutex);
        while (queue.size == 0 && running) {
            struct timespec tw;
            clock_gettime(CLOCK_REALTIME, &tw);
            tw.tv_sec += 1;
            pthread_cond_timedwait(&queue_not_empty, &queue_mutex, &tw);
        }

        if (!running) {
            pthread_mutex_unlock(&queue_mutex);
            sem_post(&kitchen_sem);
            break;
        }

       
        if (chef->id > active_chefs) {
            pthread_mutex_unlock(&queue_mutex);
            sem_post(&kitchen_sem);
            usleep(200000);
            continue;
        }

        Order *order = pq_pop(&queue);
        if (!order) {
            pthread_mutex_unlock(&queue_mutex);
            sem_post(&kitchen_sem);
            continue;
        }

        if (order->cancelled) {
            pthread_cond_signal(&queue_not_full);
            pthread_mutex_unlock(&queue_mutex);
            sem_post(&kitchen_sem);
            continue;
        }

        order->state    = STATE_PREPARING;
        order->chef_id  = chef->id;
        chef->current_order = order;

        pthread_cond_signal(&queue_not_full);
        pthread_mutex_unlock(&queue_mutex);

        log_event("Chef %d starts \"%s\" (Order #%d, %s)",
                  chef->id, order->item, order->id,
                  order->priority ? "VIP" : "Normal");

     
        int ticks = (int)(order->prep_time * 10 / sim_speed);
        for (int t = 0; t < ticks && !order->cancelled && running; t++) {
            usleep(100000); 
        }

        sem_post(&kitchen_sem);

        pthread_mutex_lock(&state_mutex);
        if (!order->cancelled) {
            order->state        = STATE_COMPLETED;
            order->completed_at = time(NULL);
            chef->orders_done++;
            log_event("Chef %d completed \"%s\" (Order #%d)",
                      chef->id, order->item, order->id);
        }
        chef->current_order = NULL;
        pthread_mutex_unlock(&state_mutex);
    }

    log_event("Chef %d left the kitchen", chef->id);
    return NULL;
}

/* ─── Waiter thread (Producer) ─────────────────────────────────────────────*/
static void *waiter_thread(void *arg) {
    (void)arg;
    srand((unsigned)time(NULL) ^ (unsigned)pthread_self());

    while (running && order_count < MAX_ORDERS) {
        /* Wait between orders (1–4 sec scaled by speed) */
        double delay = (1.0 + (rand() % 30) / 10.0) / sim_speed;
        usleep((useconds_t)(delay * 1000000));

        if (!running) break;

        pthread_mutex_lock(&queue_mutex);
        while (queue.size >= MAX_QUEUE && running) {
            log_event("Waiter: queue full, waiting…");
            pthread_cond_wait(&queue_not_full, &queue_mutex);
        }
        if (!running) { pthread_mutex_unlock(&queue_mutex); break; }

        pthread_mutex_lock(&state_mutex);
        if (order_count >= MAX_ORDERS) {
            pthread_mutex_unlock(&state_mutex);
            pthread_mutex_unlock(&queue_mutex);
            break;
        }
        int idx = order_count;
        Order *o = &all_orders[idx];
        memset(o, 0, sizeof(Order));
        int menu_idx   = rand() % MENU_SIZE;
        o->id          = next_order_id++;
        o->priority    = (rand() % 5 == 0) ? 1 : 0;  /* 20% VIP */
        strncpy(o->item, menu_items[menu_idx], 63);
        o->prep_time   = menu_times[menu_idx];
        o->state       = STATE_IN_QUEUE;
        o->created_at  = time(NULL);
        o->chef_id     = -1;
        order_count++;
        pthread_mutex_unlock(&state_mutex);

        pq_push(&queue, o);
        pthread_cond_signal(&queue_not_empty);
        pthread_mutex_unlock(&queue_mutex);

        log_event("Waiter added Order #%d \"%s\" (%s)",
                  o->id, o->item, o->priority ? "VIP" : "Normal");
    }
    log_event("Waiter finished for the evening.");
    return NULL;
}

/* ─── Dynamic chef allocator thread ─────────────────────────────────────── */
static void *allocator_thread(void *arg) {
    (void)arg;
    while (running) {
        sleep(5);
        pthread_mutex_lock(&queue_mutex);
        int qsize = queue.size;
        pthread_mutex_unlock(&queue_mutex);

        int target = active_chefs;
        if (qsize > 10 && active_chefs < MAX_CHEFS)      target = active_chefs + 1;
        else if (qsize < 3 && active_chefs > MIN_CHEFS)  target = active_chefs - 1;

        if (target != active_chefs) {
            active_chefs = target;
            sem_destroy(&kitchen_sem);
            sem_init(&kitchen_sem, 0, active_chefs);
            log_event("Allocator: adjusted chefs to %d (queue=%d)",
                      active_chefs, qsize);
        }
    }
    return NULL;
}

/* ─── Cancel-command watcher ─────────────────────────────────────────────── */
static void *cancel_watcher(void *arg) {
    (void)arg;
    while (running) {
        usleep(500000);
        FILE *f = fopen(CANCEL_FILE, "r");
        if (!f) continue;
        int cancel_id;
        if (fscanf(f, "%d", &cancel_id) == 1) {
            pthread_mutex_lock(&state_mutex);
            for (int i = 0; i < order_count; i++) {
                if (all_orders[i].id == cancel_id &&
                    all_orders[i].state != STATE_COMPLETED &&
                    !all_orders[i].cancelled) {
                    all_orders[i].cancelled = 1;
                    all_orders[i].state     = STATE_CANCELLED;
                    log_event("Order #%d \"%s\" CANCELLED",
                              cancel_id, all_orders[i].item);
                    break;
                }
            }
            pthread_mutex_unlock(&state_mutex);
        }
        fclose(f);
        remove(CANCEL_FILE);
    }
    return NULL;
}

/* ─── Speed-command watcher ──────────────────────────────────────────────── */
static void *speed_watcher(void *arg) {
    (void)arg;
    while (running) {
        usleep(500000);
        FILE *f = fopen(SPEED_FILE, "r");
        if (!f) continue;
        double spd;
        if (fscanf(f, "%lf", &spd) == 1 && spd >= 0.5 && spd <= 5.0) {
            sim_speed = spd;
            log_event("Simulation speed changed to %.1fx", sim_speed);
        }
        fclose(f);
        remove(SPEED_FILE);
    }
    return NULL;
}

/* ─── JSON state writer ──────────────────────────────────────────────────── */
static void write_state(void) {
    FILE *f = fopen(STATE_FILE ".tmp", "w");
    if (!f) return;

    pthread_mutex_lock(&state_mutex);
    pthread_mutex_lock(&log_mutex);

    fprintf(f, "{\n");
    fprintf(f, "  \"timestamp\": %ld,\n", (long)time(NULL));
    fprintf(f, "  \"sim_speed\": %.1f,\n", sim_speed);
    fprintf(f, "  \"active_chefs\": %d,\n", active_chefs);

    /* Queue snapshot */
    pthread_mutex_lock(&queue_mutex);
    fprintf(f, "  \"queue_size\": %d,\n", queue.size);
    fprintf(f, "  \"queue_orders\": [");
    for (int i = 0; i < queue.size; i++) {
        Order *o = queue.data[i];
        fprintf(f, "%s{\"id\":%d,\"item\":\"%s\",\"priority\":%d}",
                i ? "," : "", o->id, o->item, o->priority);
    }
    fprintf(f, "],\n");
    pthread_mutex_unlock(&queue_mutex);

    /* All orders */
    fprintf(f, "  \"orders\": [\n");
    for (int i = 0; i < order_count; i++) {
        Order *o = &all_orders[i];
        fprintf(f,
                "    {\"id\":%d,\"item\":\"%s\",\"priority\":%d,"
                "\"state\":%d,\"chef_id\":%d,"
                "\"created_at\":%ld,\"completed_at\":%ld,"
                "\"prep_time\":%d,\"cancelled\":%d}%s\n",
                o->id, o->item, o->priority,
                (int)o->state, o->chef_id,
                (long)o->created_at, (long)o->completed_at,
                o->prep_time, o->cancelled,
                (i < order_count - 1) ? "," : "");
    }
    fprintf(f, "  ],\n");

    fprintf(f, "  \"chefs\": [\n");
    for (int i = 0; i < MAX_CHEFS; i++) {
        Order *cur = chefs[i].current_order;
        fprintf(f,
                "    {\"id\":%d,\"active\":%d,\"orders_done\":%d,"
                "\"current_order_id\":%d,\"current_item\":\"%s\"}%s\n",
                chefs[i].id,
                (chefs[i].id <= active_chefs) ? 1 : 0,
                chefs[i].orders_done,
                cur ? cur->id  : -1,
                cur ? cur->item : "",
                (i < MAX_CHEFS - 1) ? "," : "");
    }
    fprintf(f, "  ],\n");

    /* Recent log */
    fprintf(f, "  \"log\": [\n");
    for (int i = 0; i < log_count; i++) {
        int idx = (log_head + log_count - 1 - i) % MAX_LOG_LINES;
        char esc[512];
        int j = 0, k = 0;
        while (log_lines[idx][j] && k < 500) {
            if (log_lines[idx][j] == '"')  { esc[k++] = '\\'; esc[k++] = '"'; }
            else                             { esc[k++] = log_lines[idx][j]; }
            j++;
        }
        esc[k] = 0;
        fprintf(f, "    \"%s\"%s\n", esc, (i < log_count - 1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");

    pthread_mutex_unlock(&log_mutex);
    pthread_mutex_unlock(&state_mutex);
    fclose(f);
    rename(STATE_FILE ".tmp", STATE_FILE);
}


static void *state_writer_thread(void *arg) {
    (void)arg;
    while (running) {
        write_state();
        usleep(500000); /* write every 0.5s */
    }
    write_state(); /* final flush */
    return NULL;
}


static void sig_handler(int sig) { (void)sig; running = 0; }


int main(void) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    srand((unsigned)time(NULL));
    memset(&queue, 0, sizeof(queue));
    memset(all_orders, 0, sizeof(all_orders));
    memset(chefs, 0, sizeof(chefs));

    sem_init(&kitchen_sem, 0, active_chefs);

    printf("=== Restaurant Order Management Simulator ===\n");
    printf("Chefs: %d (dynamic), Queue capacity: %d\n", active_chefs, MAX_QUEUE);
    printf("Writing state to: %s\n", STATE_FILE);
    printf("Press Ctrl+C to stop.\n\n");

  
    for (int i = 0; i < MAX_CHEFS; i++) {
        chefs[i].id = i + 1;
        pthread_create(&chefs[i].thread, NULL, chef_thread, &chefs[i]);
    }

    
    pthread_t waiter_t, alloc_t, writer_t, cancel_t, speed_t;
    pthread_create(&waiter_t,  NULL, waiter_thread,    NULL);
    pthread_create(&alloc_t,   NULL, allocator_thread, NULL);
    pthread_create(&writer_t,  NULL, state_writer_thread, NULL);
    pthread_create(&cancel_t,  NULL, cancel_watcher,   NULL);
    pthread_create(&speed_t,   NULL, speed_watcher,    NULL);

    
    while (running) {
        sleep(10);
        int received = 0, in_queue = 0, preparing = 0, completed = 0, cancelled = 0;
        pthread_mutex_lock(&state_mutex);
        for (int i = 0; i < order_count; i++) {
            switch (all_orders[i].state) {
                case STATE_RECEIVED:  received++;  break;
                case STATE_IN_QUEUE:  in_queue++;  break;
                case STATE_PREPARING: preparing++; break;
                case STATE_COMPLETED: completed++; break;
                case STATE_CANCELLED: cancelled++; break;
            }
        }
        pthread_mutex_unlock(&state_mutex);
        printf("[Summary] Total=%d  Queue=%d  Cooking=%d  Done=%d  Cancelled=%d  Chefs=%d\n",
               order_count, in_queue, preparing, completed, cancelled, active_chefs);
    }

    printf("\nShutting down…\n");
    pthread_cond_broadcast(&queue_not_empty);
    pthread_cond_broadcast(&queue_not_full);

    pthread_join(waiter_t, NULL);
    pthread_join(alloc_t,  NULL);
    pthread_join(cancel_t, NULL);
    pthread_join(speed_t,  NULL);
    for (int i = 0; i < MAX_CHEFS; i++) pthread_join(chefs[i].thread, NULL);
    pthread_join(writer_t, NULL);

    sem_destroy(&kitchen_sem);
    printf("Simulation ended. %d orders processed.\n", order_count);
    return 0;
}
