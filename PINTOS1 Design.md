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

### What Pintos Manual Says?

`devices/timer.c`에 정의된 timer_sleep() 함수를 재구현한다. 작동하는 구현이 제공되지만, 이는 "바쁜 대기", 즉 현재 시간을 확인하고 `thread_yield()`를 호출하는 루프를 돌면서 충분한 시간이 지날 때까지 기다리는 방식이다. 바쁜 대기를 피하도록 재구현한다.

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

### 




## 3.2. Priority Scheduling

## 3.3. Advanced Scheduler
