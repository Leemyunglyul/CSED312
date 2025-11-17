#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>

/* 스왑 공간 초기화 */
void swap_init (void);

/* * 데이터를 스왑 공간으로 내보냅니다 (Swap-out).
 * * 사용된 스왑 슬롯의 인덱스를 반환합니다.
 */
size_t swap_out (void *kpage);

/* * 데이터를 스왑 공간에서 메모리로 가져옵니다 (Swap-in).
 * * 'swap_index' 슬롯에서 'kpage'로 데이터를 읽어옵니다.
 */
void swap_in (size_t swap_index, void *kpage);

/* * 사용이 끝난 스왑 슬롯을 해제합니다.
 */
void swap_free (size_t swap_index);

#endif /* vm/swap.h */