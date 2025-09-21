# PINTOS 1 Design Report

Team12 한라봉 / 컴퓨터공학과 20210807 이세광 컴퓨터공학과 20210750 이명률

---

# 1. Synchronization

## 1.1. What is Synchronization?

**동기화(Synchronization)** 는 여러 스레드(또는 프로세스)가 하나의 공유 자원을 동시에 사용하려 할 때, 데이터가 꼬이거나 망가지는 문제(경쟁 상태, Race Condition)를 막기 위한 '질서 부여' 작업이다.

핵심은 **상호 배제(Mutual Exclusion)**로, 한 번에 단 하나의 스레드만 공유 자원에 접근하도록 제어하는 것입니다.

## 1.2. Synchroization Tools

대표적인 동기화 도구로는 **세마포어(Semaphore)**, **락(Lock)**, **조건 변수(Condition Variable)**  등이 있다.

### A. 세마포어 (Semaphore)


**세마포어**는 공유 자원(Shared Resource)에 대한 접근을 제어하기 위한 가장 기본적인 동기화 도구이다. 핵심 아이디어는 **'사용 가능한 자원의 개수를 나타내는 정수 카운터'** 를 사용하는 것이다.

#### Implementation

+ Struct

    ```c
    struct semaphore
    {
        unsigned value;             /* 현재 사용 가능한 자원의 수 */
        struct list waiters;        /* 자원을 기다리는 스레드들의 대기 큐 */
    };
    ```

    * `value`: 세마포어의 핵심인 정수 카운터.
    * `waiters`: `value`가 0일 때 자원을 요청한 스레드들이 block 상태로 들어가 대기하는 연결 리스트(큐). 이 큐가 있기 때문에 CPU 자원을 낭비하는 '바쁜 대기(busy-waiting)'를 하지 않고, 효율적으로 대기할 수 있다.

+ Function

    + `sema_init (struct semaphore *sema, unsigned value)`: 세마포어 구조체를 초기화한다.

    + `sema_down (struct semaphore *)`: 세마포어의 카운터를 1 감소시킨다. 이를 P 연산이라고도 한다. 자원을 획득하는 과정을 나타낸다.

    + `sema_try_down(struct semaphore *sema)`: `sema_down`의 non-blocking 버전. 기다리지 않고 즉시 자원 획득을 시도한다.

    + `sema_up(struct semaphore *sema)`: 세마포어에서 "V" 또는 "up" 연산을 수행하는 함수로, 세마포어 값을 1 증가시키고 대기 중인 스레드를 깨워주는 역할을 한다.
    
    + `sema_self_test (void)`: 동기화 도구인 세마포어가 올바르게 동작하는지 확인하는 테스트 함수입니다. 두 개의 세마포어를 이용해 서로 신호를 주고받으며, 스레드 사이의 협력 동작을 검증.
    
    + `sema_test_helper (void *sema_)`: 세마포어 두 개를 이용해 주 스레드와 상호 신호를 교환하면서 세마포어 동기화 동작을 테스트하는 보조 스레드 함수.

### B. 락 (Lock)

**락**은 **상호 배제(Mutual Exclusion)** 를 보장하기 위한 동기화 도구이다. 오직 **하나의 스레드만이 임계 구역(Critical Section)에 진입** 할 수 있도록 한다..

락은 바이너리 세마포어(값이 0 또는 1)와 매우 유사하지만, 중요한 차이점은 **소유권**이다. 세마포어는 `sema_down`한 스레드와 `sema_up`하는 스레드가 달라도 되지만, 락은 반드시 `lock_acquire`한 스레드가 `lock_release`해야 한다.

#### Implementation

+ Struct 

    ```c
    struct lock
    {
        struct thread *holder;      /* 현재 락을 소유한 스레드를 가리키는 포인터 */
        struct semaphore semaphore; /* 상호 배제를 구현하기 위한 바이너리 세마포어 */
    };
    ```

    * `holder`: 락의 '소유권' 개념을 구현하는 핵심 멤버. 현재 락을 획득한 스레드의 `struct thread`를 가리킨다. 락이 비어있으면 `NULL`이다.
    * `semaphore`: 실제 대기 및 깨우기 메커니즘은 세마포어를 통해 구현됩니다. 락은 세마포어 위에 구축된 더 특수화된 추상화 계층. `lock_init`에서 이 세마포어의 `value`를 1로 초기화하여 바이너리 세마포어로 사용한다.

+ Function

    + `lock_init (struct lock *lock)`: 락 구조체를 초기화한다.
    + `lock_acquire (struct lock *lock)`: 락을 획득하는 함수로, 임계 구역에 진입하기 위해 락을 잠그고, 락이 이미 다른 스레드에 의해 소유되어 있으면 대기하게 만든다.
    + `lock_try_acquire (struct lock *lock)`: 락을 non-blocking 방식으로 획득해보는 함수. 즉, 락이 이미 다른 스레드에 의해 소유되어 있으면 대기하지 않고 즉시 실패한다.
    + `lock_release (struct lock *lock)`: 락을 해제. 락 소유를 현재 스레드에서 없애고, 락에 연관된 세마포어 값을 증가시켜 락을 해제.
    + `lock_held_by_current_thread (const struct lock *lock)`:  현재 실행 중인 스레드가 주어진 락(lock)을 소유하고 있는지 확인한다.

### C. 조건 변수 (Condition Variable)

**조건 변수**는 어떤 **특정한 조건(Condition)이 만족될 때까지 스레드를 기다리게** 하고, 조건이 만족되었을 때 그 스레드에게 **신호(Signal)를 보내 깨우기** 위한 동기화 도구이다. 조건 변수는 **반드시 락과 함께 사용** 된다. 스레드는 공유 데이터의 상태를 확인하기 위해 먼저 락을 획득한다. 만약 원하는 조건이 아니라면, 락을 **일시적으로 해제**하고 조건 변수를 통해 잠들게 된다. 다른 스레드가 락을 획득하여 공유 데이터의 상태를 변경하고 조건을 만족시키면, 잠들어 있는 스레드에게 신호를 보낸다.

세마포어와 가장 큰 차이점은, `sema_up`은 `value`를 증가시켜 "기억"되지만, `cond_signal`은 기다리는 스레드가 없을 때 호출되면 그냥 **사라진다**(기억되지 않음).

#### Implementation

+ Struct

    ```c
    struct condition
    {
        struct list waiters; /* 조건을 기다리는 스레드들의 대기 큐 */
    };

    /* waiters 리스트에 들어갈 요소 */
    struct semaphore_elem
    {
        struct list_elem elem;      /* 리스트 요소 */
        struct semaphore semaphore; /* 이 스레드만 사용할 개인 세마포어 */
    };
    ```

    * `waiters`: 조건이 만족되기를 기다리며 잠들어 있는 스레드들을 관리하는 큐.
    * 각 대기 스레드가 자신만의 개인 세마포어(`semaphore_elem`)를 하나씩 가지고 `waiters` 큐에 들어간다. 스레드는 이 개인 세마포어를 통해 잠들고, `cond_signal`은 큐에서 하나를 꺼내 그 개인 세마포어를 `up` 시켜주는 방식으로 동작한다.

+ Function
    
    + `cond_wait (struct condition *cond, struct lock *lock)`: 조건 변수(condition variable)와 락(lock)을 이용해 스레드를 특정 조건이 만족될 때까지 기다리게 한다.

    + `cond_signal (struct condition *cond, struct lock *lock UNUSED)`: 조건 변수(condition variable)에 대해 대기 중인 스레드 중 하나를 깨운다.

    + `cond_broadcast (struct condition *cond, struct lock *lock)`: 조건 변수(condition variable)에 대기 중인 모든 스레드에게 신호를 보내 깨운다.

# 2. Analyzing the current implementation: Thread

## Data Structure

### struct thread (in `thread.h`)

```c
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */
    int priority;                       /* Priority. */
    struct list_elem allelem;           /* List element for all threads list. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element. */

#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /* Page directory. */
#endif

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };
```

스레드에 대한 모든 정보를 담고 있는 TCB (Thread Control Block). 주요 멤버는 다음과 같다.

`tid_t tid`: 스레드를 유일하게 식별하는 **스레드 식별자(ID)**.

`enum thread_status status`: 스레드의 현재 상태. (RUNNING, READY, BLOCKED, DYING)

`char name[16]`: 디버깅 목적으로 사용되는 스레드의 이름.

`uint8_t *stack`: 스레드의 커널 스택 포인터. 문맥 교환 시 이 스택의 위치가 저장됨.

`int priority`: 스레드의 우선순위. 스케줄러가 다음 실행할 스레드를 결정하는 데 사용.

`struct list_elem elem, allelem`:

+ `elem`: 이중 목적을 가집니다. 스레드가 READY 상태일 때는 ready_list에 연결되고, BLOCKED 상태일 때는 세마포어 등의 waiters 리스트에 연결됩니다. 두 상태는 상호 배타적이므로 하나의 멤버로 처리가 가능합니다.

+ `allelem`: 모든 스레드를 관리하는 all_list에 연결되기 위한 요소입니다.

+ `unsigned magic`: 스택 오버플로우(Stack Overflow) 감지를 위한 '매직 넘버'. 스택이 너무 많이 자라서 struct thread 영역을 침범하면 이 값이 변하게 되고, thread_current() 함수가 이를 감지하여 시스템 오류를 알려줌.

### 전역 변수 (in thread.c)

`static struct list ready_list`: Ready Queue. READY 상태의 스레드들을 관리하는 리스트. 스케줄러는 이 리스트에서 다음에 실행할 스레드를 꺼내온다.

`static struct list all_list`: 시스템에 존재하는 모든 스레드를 관리하는 리스트. thread_foreach 같은 함수에서 모든 스레드를 순회할 때 사용됨.

`static struct thread *idle_thread`: 실행할 스레드가 아무것도 없을 때 실행되는 idle 스레드를 가리키는 포인터.

`static struct lock tid_lock`: 새로운 tid를 할당할 때 발생하는 경쟁 상태를 막기 위한 락.

## 스레드의 생명주기와 상태 변화 (Thread Lifecycle & State Transitions)

`enum thread_status`에 정의된 4가지 상태를 중심으로 스레드의 생명주기가 결정된다.

RUNNING (실행): 현재 CPU를 점유하여 코드를 실행 중인 상태.

READY (준비): 언제든지 실행될 수 있지만, 다른 스레드가 실행 중이어서 대기하는 상태. ready_list에 포함된다.

BLOCKED (대기): 특정 이벤트(예: I/O 완료, Lock 해제)를 기다리며 잠들어 있는 상태. ready_list에 포함되지 않는다.

DYING (종료 중): 실행이 끝나고 소멸을 기다리는 상태.

주요 상태 전이 함수:

+ `thread_create()` → **READY**: 스레드가 생성되면 ready_list에 추가되어 실행을 기다린다.

+ `schedule()` → **RUNNING**: 스케줄러가 ready_list에서 스레드를 선택하여 실행시킨다.

+ `thread_block()` → **BLOCKED**: 실행 중인 스레드가 lock_acquire, sema_down 등으로 인해 스스로를 블록시킨다.

+ `thread_unblock()` → **READY**: BLOCKED 상태의 스레드가 다른 스레드에 의해 깨어나 ready_list에 추가된다.

+ `thread_yield()` → **READY**: 실행 중인 스레드가 자발적으로 CPU를 양보하고 ready_list의 맨 뒤로 들어간다.

+ `thread_exit()` → **DYING**: 스레드가 실행을 마치고 종료 상태로 들어간다. 이후 schedule() 과정에서 메모리가 완전히 해제된다.

## 주요 기능별 함수 분석

### 초기화

`thread_init()`: 스레드 시스템을 초기화하는 핵심 함수로, 초기 실행 중인(main) 스레드의 구조체를 설정하고 스레드 관리와 관련된 각종 내부 자료구조(락, 리스트 등)를 준비.

`thread_start()`: idle thread(항상 ready 큐에 존재, 실질적으로 할 일 없음)를 생성하여 스케줄러의 예외 처리를 단순하게 만들고, 선점형 스케줄링을 활성화하며, idle thread가 완전히 준비될 때까지 대기한다. 이를 통해 스레드 관리 시스템의 안전성과 일관성을 보장한다.

* Idle Thread

  항상 가장 낮은 우선순위로 동작하며, ready 큐에 실행 가능한 스레드가 없을 때만 실질적으로 CPU를 점유한다. 운영체제의 스케줄러가 특별한 예외상황을 처리하지 않고, 언제나 실행 가능한 스레드(idle thread)가 존재한다는 전제로 단순하고 일관적인 구조를 유지할 수 있다.idle thread가 존재하지 않으면, ready queue가 비어있을 때 스케줄러 오류가 발생할 수 있다.

### 스레드 생성 및 종료

`thread_create()`: 새로운 커널 스레드를 생성하는 역할을 한다. 이 함수는 이름, 우선순위, 실행할 함수, 그리고 함수에 넘겨줄 추가 인자를 받고, 준비 큐에 새 스레드를 추가하여 실행 준비 상태로 만든다. 성공하면 새 스레드의 ID를 반환하고 실패 시 TID_ERROR를 반환한다.

1. `palloc_get_page()`: 스레드 구조체와 커널 스택을 위한 4KB 페이지를 할당받는다.

2. `init_thread()`: 할당받은 메모리에 스레드 이름, 우선순위, 상태(BLOCKED) 등 기본 정보를 설정하고 all_list에 추가한다.

3. 스택 프레임 구성: 새로운 스레드가 처음 실행될 때 필요한 문맥(context)을 스택에 미리 쌓아둔다. switch_threads, switch_entry, kernel_thread 순으로 실행될 수 있도록 스택을 조작한다.

4. `thread_unblock()`: 초기화가 끝난 스레드를 READY 상태로 만들어 ready_list에 넣어 스케줄링 대상이 되게 한다.

`thread_exit()`: 현재 실행 중인 스레드를 종료시키고 시스템에서 제거한다. 이 함수는 호출한 스레드가 더 이상 실행되지 않도록 스케줄러에게 알리고, 스레드 상태를 THREAD_DYING으로 설정한 후 다음 실행할 스레드를 스케줄링한다. 이 함수는 절대 호출한 곳으로 돌아가지 않는다.

1. 자신을 all_list에서 제거하고 상태를 DYING으로 바꿉니다.

2. `schedule()`을 호출하여 다른 스레드에게 CPU를 넘깁니다.

3. 이 함수는 절대 리턴하지 않습니다. 이후 이 스레드의 메모리는 `thread_schedule_tail`에서 다른 스레드에 의해 해제됩니다.

### 스케줄링 및 문맥 교환

`schedule()`: 현재 실행 중인 스레드를 실행 대기 상태가 아닌 상태로 변경한 후, 운영체제의 스케줄러가 실행할 다음 스레드를 선택하고 컨텍스트 전환을 수행한다.

1. `next_thread_to_run()`: ready_list에서 다음에 실행할 스레드를 선택한다. 현재는 FIFO로 구현되어 있다.

2. `switch_threads()`: 현재 스레드(cur)에서 다음 스레드(next)로 문맥 교환을 수행한다.

`thread_tick()`: 타이머 인터럽트가 발생할 때마다 호출된다.

1. 시스템 통계(idle_ticks 등)를 업데이트.

2. thread_ticks를 1 증가시켜 현재 스레드가 실행된 시간을 센다.

3. 만약 thread_ticks가 TIME_SLICE(기본 4틱)에 도달하면, **선점(Preemption)** 이 필요함을 표시하여 인터럽트가 끝날 때 스케줄링이 일어나게 한다.

`thread_yield()`: 현재 스레드가 자발적으로 CPU를 양보한다. 자신을 ready_list에 넣고 schedule()을 호출하여 다른 스레드를 실행시킨다.

`thread_schedule_tail()`: 문맥 교환이 완료된 후에 호출된다. 이전 스레드(prev)가 DYING 상태였다면, 이 함수에서 `palloc_free_page(prev)`를 호출하여 이전 스레드의 메모리를 안전하게 해제한다.

# 3. Proposed design

## 3.1. Alarm Clock

### What PINTOS Manual Says?

`devices/timer.c`에 정의된 timer_sleep() 함수를 재구현한다. 작동하는 구현이 제공되지만, 이는 "바쁜 대기", 즉 현재 시간을 확인하고 `thread_yield()`를 호출하는 루프를 돌면서 충분한 시간이 지날 때까지 기다리는 방식이다. 바쁜 대기를 피하도록 재구현한다.

### Why Alarm Clock?

busy waiting을 알람 시계(alarm clock) 방식으로 바꿔야 하는 주요 이유는 **CPU 자원 낭비를 막고 시스템 효율성을 높이기 위해서이다.**

**Busy waiting**은 특정 조건이 충족될 때까지 스레드가 무의미한 반복문을 돌며 지속적으로 CPU를 점유하는 방식이다. 
**알람 시계** 방식은 스레드가 기다려야 할 시간이 될 때까지 스스로를 **수면(sleep) 상태**로 만들고, 타이머 인터럽트가 발생하면 깨어나도록 하는 방식이다.

결론적으로, 바쁜 대기 방식은 **자원을 낭비하고 비효율적**이지만, 알람 시계 방식은 **CPU 자원을 효율적으로 사용하고 시스템 성능을 최적화**하는 원칙을 따른다.


```c
/* Sleeps for approximately TICKS timer ticks.  Interrupts must
   be turned on. */
void
timer_sleep (int64_t ticks) 
{
  int64_t start = timer_ticks ();

  ASSERT (intr_get_level () == INTR_ON);
  while (timer_elapsed (start) < ticks) 
    thread_yield ();
}
```

현재 코드의 문제점은 다음과 같다.

Busy Waiting: while 루프는 계속해서 `timer_elapsed()`를 호출하여 시간을 확인한다. 비록 `thread_yield()`를 통해 다른 스레드에게 CPU를 양보하지만, 이 "잠자는" 스레드는 여전히 Ready 상태로 Ready Queue에 남아있다.

CPU 자원 낭비: 스케줄러는 CPU를 할당할 때마다 Ready Queue에 있는 이 스레드를 불필요하게 고려해야 한다. 스레드는 깨어나자마자 시간을 체크하고 다시 양보하는 일을 반복하며 CPU 사이클을 낭비한다.

스케줄러 부하: 불필요한 문맥 교환(Context Switching)이 계속 발생하여 시스템 전체에 부하를 줍니다.

### Our Design: Alarm Clock(Sleep/Wakeup)

효율적인 방법은 스레드를 Blocked 상태로 만들어 Ready Queue에서 완전히 제외하는 것이다. 스레드는 정해진 시간이 될 때까지 CPU 경쟁에 참여하지 않다가, 시간이 되면 깨어나(unblock) 다시 Ready Queue로 돌아가야 한다.

이를 위한 전체적인 흐름은 다음과 같다.

Sleep 요청: 스레드가 `timer_sleep(n)`을 호출하면, "지금부터 n 틱 후에 깨워달라"고 요청한다.

깨어날 시간 기록: 스레드가 깨어나야 할 정확한 tick 값을 계산하여 어딘가에 저장한다.

대기 리스트 등록 및 Block: 스레드를 잠자는 스레드들을 관리하는 별도의 리스트(예: sleep_list)에 추가하고, `thread_block()`을 호출하여 스스로를 Blocked 상태로 만든다.

시간 확인 (Timer Interrupt): 매 타이머 틱마다 발생하는 `timer_interrupt` 핸들러가 sleep_list를 확인한다.

Wakeup 처리: 현재 시간이 sleep_list에 있는 스레드가 깨어나기로 한 시간과 같거나 지났다면, 해당 스레드를 sleep_list에서 제거하고 `thread_unblock()`을 호출하여 Ready Queue로 옮긴다.

## Our Implementation

### threads/thread.h

```c
struct thread{
    int64_t wakeup_tick; 
};
```
`struct thread`에 스레드가 깨어날 시간을 저장할 멤버를 추가해야 한다.

### devices/timer.c

1. 전역 변수 추가

    ```c
    static struct list sleep_list;
    ```

    잠자는 스레드들을 관리할 리스트를 static 전역 변수로 선언한다.

2. `timer_init()` 함수 수정

    ```c
    void
    timer_init (void) 
    {
    pit_configure_channel (0, 2, TIMER_FREQ);
    intr_register_ext (0x20, timer_interrupt, "8254 Timer");

    /* sleep_list 초기화 */
    list_init(&sleep_list);
    }
    ```

    `timer_init()`과 함께 sleep_list를 초기화해야 한다.

3. `timer_sleep()` 재구현

    ```c
    void
    timer_sleep (int64_t ticks) 
    {
    if (ticks <= 0) {
        return;
    }

    enum intr_level old_level = intr_disable ();
    
    struct thread *cur = thread_current ();
    
    cur->wakeup_tick = timer_ticks () + ticks;
    
    list_insert_ordered (&sleep_list, &cur->elem, wake_less, NULL);
    
    thread_block ();
    
    intr_set_level (old_level);
    }
    ```

    현재 스레드가 깨어나야할 시간을 계산하여 저장하고 스레드를 sleep_list에 추가하고 block 상태로 만든다. 나중에 unblock되면 다시 실행 재개를 한다.

    * 
        ```c
        static bool
        wake_less (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
        {
            const struct thread *thread_a = list_entry (a, struct thread, elem);
            const struct thread *thread_b = list_entry (b, struct thread, elem);
            return thread_a->wakeup_tick < thread_b->wakeup_tick;
        }
        ```
        위 커스텀 함수와 이미 구현된 `list_insert_ordered()`로 `sleep_list`를 오름차순으로 만든다.


4. `timer_interrupt()` 함수 수정

    ```c
    static void
    timer_interrupt (struct intr_frame *args UNUSED)
    {
    ticks++;
    thread_tick ();

    if(list_empty (&sleep_list)) {
        return;
    }

    struct list_elem *e = list_begin (&sleep_list);

    while(e != list_end (&sleep_list)) {
        struct thread *t = list_entry (e, struct thread, elem);
        
        if(ticks >= t->wakeup_tick) {
        struct list_elem *next = list_next (e);
        list_remove (e);
        thread_unblock (t);
        e = next;
        } else {
        break;
        }
    }
    }
    ```

    기존 주기적으로 호출되어 시스템 틱(tick) 을 증가시키는 것에 더해, 일정 시간이 지난 후 깨워야 할 스레드를 관리하게 한다. 

## 3.2. Priority Scheduling

### What PINTOS Manual says?

현재 실행 중인 스레드보다 더 높은 우선순위를 가진 스레드가 준비 큐(ready list)에 추가되면, 현재 스레드는 즉시 새로운 스레드에게 프로세서(CPU)를 양보해야 한다. 마찬가지로, 스레드들이 락(lock), 세마포(semaphore), 또는 조건 변수(condition variable)를 기다릴 때에는, 기다리는 스레드들 중 가장 높은 우선순위를 가진 스레드가 먼저 깨어나야 한다. 스레드는 언제든지 자신의 우선순위를 높이거나 낮출 수 있지만, 우선순위를 낮춘 결과 더 이상 가장 높은 우선순위를 갖지 않게 될 경우, 즉시 CPU를 양보해야 한다.

스레드의 우선순위는 PRI_MIN(0)부터 PRI_MAX(63)까지의 범위를 가진다. 숫자가 낮을수록 낮은 우선순위를 의미한다. 스레드의 초기 우선순위는 `thread_create()` 함수의 인자로 전달됩니다. 다른 우선순위를 선택할 특별한 이유가 없다면 PRI_DEFAULT(31)를 사용한다. PRI_ 매크로들은 threads/thread.h에 정의되어 있으며, 이 값들을 변경해서는 안 된다.

우선순위 스케줄러가 만족해야 할 3가지 핵심 규칙

+ 선점 (Preemption): 더 높은 우선순위의 스레드가 실행 가능해지면(ready list에 들어오면) 즉시 현재 실행 중인 스레드를 멈추고 CPU를 차지해야 한다.

+ 우선순위 기반의 Wake-up: 잠들어 있던 여러 스레드가 동시에 깨어날 때, 우선순위가 가장 높은 스레드부터 깨어나야 한다.

+ 우선순위 변경 시 선점: 스레드가 스스로 우선순위를 낮춘 경우, 자신보다 우선순위가 높은 다른 스레드가 있다면 즉시 CPU를 양보해야 한다.

#### 문제점: 우선순위 역전 (Priority Inversion)

우선순위 스케줄링의 한 가지 문제는 **"우선순위 역전(priority inversion)"**이다. 높은, 중간, 낮은 우선순위를 가진 스레드를 각각 H, M, L이라고 가정해 보자. 만약 H가 L이 점유한 락을 기다려야 하고, M이 준비 큐에 있다면, H는 결코 CPU를 얻지 못할 것이다. 왜냐하면 낮은 우선순위의 스레드 L이 CPU 시간을 얻지 못해 락을 놓아주지 못하기 때문이다.

우선순위 역전은 우선순위가 높은 스레드가 자신보다 낮은 스레드의 작업이 끝나기를 기다리느라 아무 일도 못 하는 비효율적인 상황을 말한다.

#### 해결책: 우선순위 기부 (Priority Donation)

이 문제에 대한 부분적인 해결책은, L이 락을 보유하고 있는 동안 H가 자신의 우선순위를 L에게 **"기부(donate)"**하고, L이 락을 해제하면(그리하여 H가 락을 획득하면) 기부를 철회하는 것이다.

우선순위 기부가 필요한 모든 다양한 상황들을 고려해야 합니다. 여러 스레드가 하나의 스레드에게 우선순위를 기부하는 **다중 기부(multiple donations)** 를 반드시 처리해야 한다. 또한 **중첩된 기부(nested donation)**도 처리해야 한다. 예를 들어, H가 M이 보유한 락을 기다리고, M은 L이 보유한 락을 기다린다면, M과 L은 모두 H의 우선순위로 올라가야 한다. 

우선순위 기부는 락을 기다리는 스레드가 자신의 높은 우선순위를 락을 보유한 낮은 우선순위의 스레드에게 일시적으로 빌려주는 것이다.

#### 구현 목표

`void thread_set_priority (int new_priority)`: 현재 스레드의 우선순위를 new_priority로 설정한다.
만약 이 변경으로 인해 현재 스레드가 더 이상 가장 높은 우선순위가 아니게 되면, CPU를 양보(yield)한다.

`int thread_get_priority (void)`: 현재 스레드의 우선순위를 반환합니다. 우선순위 기부를 받은 상태라면, 더 높은 (기부받은) 우선순위를 반환해야 합니다.

### Our Desgin

현재의 scheduling 방식은 thread의 우선순위를 고려하지 않고, FIFO 기반으로 생성 순서에 따라 Round-Robin 방식을 채택하고 있다.
스레드의 중요도에 따라 CPU 자원을 효과적으로 배분하고, 시스템 응답성을 높이며, 다양한 운영체제 시나리오를 제대로 지원하기 위해서 우선순위 스케줄러를 구현한다.

### Our Implementation

Priority Scheduler 구현 핵심은 (1) Ready 리스트를 우선순위 큐로 만들기, (2) 우선순위 기부 구현하기 두 가지이다.

#### 0. 자료구조 준비

```c
struct thread{
    int base_priority;                  /* 기부 받기 전의 원래 우선순위 */
    struct lock *waiting_lock;          /* 현재 이 스레드가 기다리고 있는 lock */
    struct list donations;              /* 이 스레드에게 우선순위를 기부한 스레드들의 리스트 */
    struct list_elem donation_elem;     /* 다른 스레드의 donations 리스트에 들어갈 때 사용할 element */
}
```

우선순위 스케줄링과 기부에 필요한 정보를 `struct thread`에 추가해야 한다.

---

```c
// synch.h

struct lock {
    struct thread *holder;      /* Thread holding lock (for debugging). */
    struct semaphore semaphore; /* Binary semaphore controlling access. */

    struct list waiters; 
};
```

추가로 `strcut lock`에 lock을 기다리는 thread list를 만든다.

#### 1. Ready 리스트를 우선순위 큐로 만들기

현재 `ready_list`는 단순한 FIFO(선입선출) 큐이다. 이것을 우선순위가 높은 스레드가 항상 먼저 나올 수 있도록 수정해야 한다. 가장 좋은 방법은 `ready_list`를 항상 우선순위 순으로 정렬된 상태로 유지하는 것이다. 

`thread_unblock()`, `thread_yield()` 두 함수 내부에 있는 list_push_back (&ready_list, ...) 코드를 list_insert_ordered()로 변경해야 한다.

```c
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_ordered (&ready_list, &t->elem, priority_less, NULL);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}
```

`thread_yield()`도 이와 마찬가지로 `list_insert_ordered()`로 변경한다.

`synch.c`에서도 thread를 대기시키는 곳이 있는데, 이 부분도 마찬가지로 모두 수정해야 한다.
`sema_down()`, `cond_wait()`, `cond_signal()`이다.

`sema_down()`, `cond_wait()`은 위와 동일하게 `list_insert_ordered()`로 변경하고,
`cond_signal()`은 아래와 같이 비교함수를 추가하여 정렬한다.


```c
// synch.c

bool conditional_var_comparator(struct list_elem *a,struct list_elem *b, void *aux){

  struct semaphore_elem *semaphore_one = list_entry(a,struct semaphore_elem,elem);
  struct semaphore_elem *semaphore_two = list_entry(b,struct semaphore_elem,elem);

  struct thread *s_one= list_entry(list_front(&semaphore_one->semaphore.waiters), struct thread, elem);
  struct thread *s_two= list_entry(list_front(&semaphore_two->semaphore.waiters), struct thread, elem);

  if(s_one->priority > s_two->priority)
    return true;
  else return false;
}


void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      list_insert_ordered(&sema->waiters, &thread_current()->elem, priority_less, NULL);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

```

---

```c
bool
priority_less (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
    const struct thread *thread_a = list_entry (a, struct thread, elem);
    const struct thread *thread_b = list_entry (b, struct thread, elem);
    return thread_a->priority > thread_b->priority;
}
```

`list_insert_ordered`에 쓰일 우선순위 비교를 위한 함수도 추가하여 준다.

#### 2. 우선순위 기부 구현하기

우선순위 기부를 처리하려면 스레드의 기본(base) 우선순위와 기부받은 것까지 포함한 실질(effective) 우선순위를 구분해야 한다.

```c
static void
init_thread (struct thread *t, const char *name, int priority){
    t->priority = priority;

    t->base_priority = priority;
    t->waiting_lock = NULL;
    list_init (&t->donations);
}
```

먼저, 0단계에서 추가한 멤버들을 초기화한다.

---

더 높은 우선순위의 스레드가 실행 가능 상태가 되면, 현재 실행 중인 스레드는 즉시 CPU를 양보해야 한다.

```c
static void
test_max_priority (void)
{
  if (list_empty(&ready_list))
    return;
  
  struct thread *next_thread = list_entry(list_front(&ready_list), struct thread, elem);
  if (thread_current()->priority < next_thread->priority)
    {
      thread_yield();
    }
}
```

현재 실행중인 스레드보다 ready_list에 있는 가장 우선순위 높은 스레드가 더 높은 우선순위를 가질 경우 CPU를 양보하도록 하는 함수이다.
이 함수를 `thread_create`에 추가하여준다.

```c
tid_t
thread_create (const char *name, int priority, thread_func *function, void *aux) {
/* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);
  test_max_priority();

  return tid;
}
```

---


```c
void
thread_set_priority (int new_priority) 
{
  if (thread_mlfqs) {
    return;
  }

  thread_current ()->base_priority = new_priority;
  thread_recalculate_priority(thread_current()); // 기부 상태를 고려하여 실질 우선순위 재계산
  test_max_priority(); // 선점 확인
}
```
이 함수는 이제 스레드의 '기본' 우선순위를 바꾸고, 그에 따라 실질 우선순위를 재계산한 뒤 선점을 확인해야 한다.

`thread_get_priority()`의 경우는 thread_current ()->priority를 반환하면 되므로, 수정할 필요가 없다. 이미 priority 멤버가 실질 우선순위를 나타내도록 관리되기 때문이다.

```c
void
thread_recalculate_priority (struct thread *t)
{
  int max_priority = t->base_priority;

  if (!list_empty(&t->donations))
  {
    struct list_elem *e;
    for (e = list_begin(&t->donations); e != list_end(&t->donations); e = list_next(e))
    {
      struct thread *donor = list_entry(e, struct thread, donation_elem);
      if (donor->priority > max_priority) {
        max_priority = donor->priority;
      }
    }
  }

  t->priority = max_priority;
}
```

우선순위 기부(priority donation) 메커니즘에 따라 스레드의 실행 우선순위를 재계산한다.
여러 스레드가 기부한 우선순위를 종합하여, 최종적으로 반드시 가장 높은 우선순위를 가지도록 보장한다.

---

```c
void
thread_donate_priority (struct thread *holder)
{
  if (holder == NULL) return;
  
  struct thread *donor = thread_current();
  
  list_push_back(&holder->donations, &donor->donation_elem);
  
  while(holder) {
    thread_recalculate_priority(holder);
    if (holder->waiting_lock) {
      holder = holder->waiting_lock->holder;
    } else {
      break;
    }
  }
}
```

현재 실행 중인 스레드(기부자)의 우선순위를 락을 점유하고 있는 스레드(소유자)에게 기부한다.
이 과정에서 우선순위 역전 문제를 완화하기 위해, 락 대기 중인 여러 스레드에 걸쳐 우선순위 기부가 연쇄적으로 전파되는 구조를 채택했다.


```c
//synch.c

void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  struct thread *cur = thread_current();

  if (lock->holder != NULL)
  {
    cur->waiting_lock = lock;
    thread_donate_priority(lock->holder);
  }

  sema_down (&lock->semaphore);
  lock->holder = cur;
  cur->waiting_lock = NULL;
}
```

만약 lock이 다른 스레드에 의해 소유되고 있으면, 현재 스레드는 이 락을 기다리고 있다고 표시하고
락을 가진 스레드에게 현재 스레드 우선순위를 기부한다.

---

```c
void
thread_remove_donations_for_lock (struct lock *lock)
{
  struct thread *cur = thread_current();
  struct list_elem *e;

  for (e = list_begin(&cur->donations); e != list_end(&cur->donations); )
  {
    struct thread *donor = list_entry(e, struct thread, donation_elem);
    if (donor->waiting_lock == lock) {
      e = list_remove(e);
    } else {
      e = list_next(e);
    }
  }
}
```
현재 스레드가 가지고 있는 우선순위 기부 목록(donations) 중 특정 락(lock)에 관련된 기부를 제거하는 기능을 수행한다.
락을 해제하거나 더 이상 기부가 필요 없을 때 해당 락에 대한 기부 기록을 제거하는 용도로 사용된다.

```c
// synch.c

void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  struct thread *cur = thread_current();

  thread_remove_donations_for_lock(lock);
  thread_recalculate_priority(cur);

  lock->holder = NULL;
  sema_up (&lock->semaphore);
}
```
현재 스레드가 가지고 있는 락을 해제하고, 우선순위 기부 상태를 갱신하는 역할을 수행한다.


## 3.3. Advanced Scheduler

### What PINTOS Manual says?

4.4BSD 스케줄러와 유사한 다단계 피드백 큐 스케줄러를 구현해야 한다. 이 스케줄러는 시스템에서 실행 중인 작업들의 평균 응답 시간을 줄이는 것이 목적이다. 우선순위 스케줄러처럼, 고급 스케줄러도 우선순위에 따라 실행할 스레드를 선택하지만 우선순위 기부(priority donation)는 하지 않는다.

### Why Adnvaced Scheduler?

기존 우선순위 스케줄러의 문제점은 **"기아 상태(Starvation)"**이다. 우선순위가 낮은 스레드는 높은 스레드가 계속 나타나면 영원히 실행되지 못할 수 있다.

**Advanced Scheduler(MLFQS)**는 이 문제를 해결하기 위해 다음 두 가지 철학을 따른다.

최근에 CPU를 많이 사용한 스레드는 인기가 없다: 이런 스레드는 다른 스레드를 위해 양보해야 하므로, 우선순위를 낮춘다. (주로 계산 위주의 작업)

최근에 CPU를 거의 사용하지 않은 스레드는 중요하다: 이런 스레드는 사용자의 입력을 기다리는 등 상호작용(interactive) 작업일 가능성이 높으므로, 우선순위를 높여서 빨리 반응할 수 있게 해준다.

결론적으로, 모든 스레드에게 공평한 기회를 주되, 응답성이 중요한 스레드를 우대하는 것이 이 스케줄러의 목표입니다.

#### How to implement?

이 동적인 우선순위 조절은 세 가지 변수를 통해 이루어진다.

nice: 스레드가 얼마나 "착한지"를 나타내는 값. 사용자가 스레드의 우선순위에 영향을 줄 수 있는 유일한 방법이다. 값이 높을수록 이타적이므로 우선순위가 낮아지고, 낮을수록(음수) 이기적이므로 우선순위가 높아진다.

recent_cpu: 스레드가 최근에 얼마나 많은 CPU 시간을 사용했는지를 나타내는 값이다. 시간이 지남에 따라 점차 감소(decay)하며, 이 값이 높을수록 우선순위는 낮아진다.

load_avg: 시스템 전체가 얼마나 바쁜지를 나타내는 값. 현재 실행 가능한(Ready 또는 Running) 스레드의 수에 따라 결정되며, recent_cpu가 얼마나 빨리 감소할지에 영향을 준다.

이 변수들을 이용해 스레드의 priority를 주기적으로 재계산하게 된다.

*고정 소수점 연산 (Fixed-Point Arithmetic)*: Pintos 커널에서는 부동 소수점(floating point) 연산을 사용할 수 없다. 하지만 위의 세 가지 변수들을 계산하는 공식에는 소수 연산이 필요하다. 이 문제를 해결하기 위해 고정 소수점(Fixed-Point) 방식을 직접 구현해야 한다. 이 연산을 쉽게 하기 위해, threads/fixed-point.h 라는 새 헤더 파일을 만들고 매크로를 쓰는 것이 편하다

### Our Implementation

#### 고정 소수점 연산

```c
// fixed-point.h

#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H

#define F (1 << 14)

#define INT_TO_FP(n) ((n) * F)

#define FP_TO_INT_ZERO(x) ((x) / F)

#define FP_TO_INT_NEAREST(x) ((x) >= 0 ? ((x) + F / 2) / F : ((x) - F / 2) / F)

#define FP_ADD(x, y) ((x) + (y))
#define FP_SUB(x, y) ((x) - (y))
#define FP_ADD_INT(x, n) ((x) + (n) * F)
#define FP_SUB_INT(x, n) ((x) - (n) * F)

#define FP_MUL(x, y) (((int64_t)(x)) * (y) / F)
#define FP_MUL_INT(x, n) ((x) * (n))
#define FP_DIV(x, y) (((int64_t)(x)) * F / (y))
#define FP_DIV_INT(x, n) ((x) / (n))

#endif 
```

위에서 요구한대로 새로이 fixed-point.h를 만들고 매크로를 사용한다.

```txt
Convert n to fixed point:	n * f
Convert x to integer (rounding toward zero):	x / f
Convert x to integer (rounding to nearest):	(x + f / 2) / f if x >= 0,
(x - f / 2) / f if x <= 0.
Add x and y:	x + y
Subtract y from x:	x - y
Add x and n:	x + n * f
Subtract n from x:	x - n * f
Multiply x by y:	((int64_t) x) * y / f
Multiply x by n:	x * n
Divide x by y:	((int64_t) x) * f / y
Divide x by n:	x / n
```

위의 핀토스 메뉴얼의 Reference Guide B.6.을 참고하여 코드를 작성하였다.

#### 자료구조 추가


```c
//thread.h

struct thread{
  int nice;
  int recent_cpu;
}
```

```c
//thread.c

static int load_avg;
```

우선순위 조절을 위한 세 가지 변수 nice, recent_cpu, load_avg를 추가하여 준다.

```c
void
thread_init (void) 
{
  load_avg = 0;
}
```

`load_avg`는 `thread_init()`에서 추가하여 준다.

#### MLFQS 스케줄러 구현

```c
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  if (thread_mlfqs)
  {
    /* 매 틱마다: 현재 스레드(idle 제외)의 recent_cpu 1 증가 */
    if (t != idle_thread)
      t->recent_cpu = FP_ADD_INT(t->recent_cpu, 1);

    /* 매 4틱마다: 모든 스레드의 priority 재계산 */
    if (timer_ticks() % TIME_SLICE == 0)
      thread_foreach(mlfqs_recalculate_priority, NULL);

    /* 매 1초마다: load_avg와 모든 스레드의 recent_cpu 재계산 */
    if (timer_ticks() % TIMER_FREQ == 0)
    {
      mlfqs_recalculate_load_avg();
      thread_foreach(mlfqs_recalculate_recent_cpu, NULL);
    }
  }

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}
```

기존 코드에서 `if(thread_mlfqs)` 부분이 추가되었다.

핀토스 메뉴얼의 Reference Guide B.4.4BSD Scheduler를 참고하면,

+ 매 틱마다 현재 스레드의 recent_cpu를 1씩 증가

+ 매 4 틱마다 모든 스레드에 대해 우선순위 재계산

+ 매 1초마다 load_avg와 모든 스레드의 recent_cpu 재계산

다음과 같은 내용이 나와있어 이를 적용하였다.

```c
static void
mlfqs_recalculate_priority (struct thread *t, void *aux UNUSED)
{
  if (t != idle_thread)
  {
    int term1 = FP_TO_INT_NEAREST (FP_DIV_INT (t->recent_cpu, 4));
    int term2 = t->nice * 2;
    int new_priority = PRI_MAX - term1 - term2;

    if (new_priority > PRI_MAX)
      t->priority = PRI_MAX;
    else if (new_priority < PRI_MIN)
      t->priority = PRI_MIN;
    else
      t->priority = new_priority;
  }
}

static void
mlfqs_recalculate_recent_cpu (struct thread *t, void *aux UNUSED)
{
  if (t != idle_thread)
    {
      int load_x_2 = FP_MUL_INT (load_avg, 2);
      int decay_coeff = FP_DIV (load_x_2, FP_ADD_INT (load_x_2, 1));
      
      t->recent_cpu = FP_ADD_INT (FP_MUL (decay_coeff, t->recent_cpu), t->nice);
    }
}

static void
mlfqs_recalculate_load_avg (void)
{
  int ready_threads = list_size (&ready_list);
  if (thread_current () != idle_thread)
      ready_threads++;

  int term1 = FP_MUL (FP_DIV_INT (INT_TO_FP (59), 60), load_avg);
  int term2 = FP_MUL_INT (FP_DIV_INT (INT_TO_FP (1), 60), ready_threads);
  
  load_avg = FP_ADD (term1, term2);
}
```

위는 MLFQS 헬퍼 함수들이다. 기존 우선순위 스케줄러와 달리 Donation logic도 제외하여야 하고 recent_cpu와 load_avg는
재계산이 필요한 작업이므로 다음과 같은 헬퍼 함수를 추가하였다.

---

```c
/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) 
{
  struct thread *cur = thread_current();
  cur->nice = nice;
  mlfqs_recalculate_priority(cur, NULL);
  test_max_priority();
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  return FP_TO_INT_NEAREST(FP_MUL_INT(load_avg, 100));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  return FP_TO_INT_NEAREST(FP_MUL_INT(thread_current()->recent_cpu, 100));
}
```

MLFQS 스케줄러의 인터페이스 함수들은 다음과 같이 구현하였다.

#### Donation 비활성화

```c
void
lock_acquire (struct lock *lock)
{
  if (thread_mlfqs) {
      sema_down(&lock->semaphore);
      lock->holder = thread_current();
      return;
  }
}

void
lock_release (struct lock *lock) 
{
  if (thread_mlfqs) {
      lock->holder = NULL;
      sema_up(&lock->semaphore);
      return;
  }
}
```

3.2.의 우선순위 스케줄러와 다르게 여기서는 donation 고려하지 않으므로 이 logic을 동기화에도 추가하여준다. 
