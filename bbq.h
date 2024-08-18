#include <atomic>
#include <cstddef>
#include <cstdlib>

#pragma once

namespace PEX {
namespace BBQ {
namespace SPSC {

#define bbq_likely(x)   (__builtin_expect(!!(x),true))
#define bbq_unlikely(x) (__builtin_expect(!!(x),false))
#define bbq_load_rlx(x) std::atomic_load_explicit(&x, std::memory_order_relaxed);
#define bbq_load_acq(x) std::atomic_load_explicit(&x, std::memory_order_acquire);
#define bbq_store_rlx(x, v) std::atomic_store_explicit(&x, v, std::memory_order_relaxed);
#define bbq_store_rel(x, v) std::atomic_store_explicit(&x, v, std::memory_order_release);

/* Block based queue with capacity of N and B blocks */
template<class T, size_t N, size_t B>
class Queue {

    /* Each block contains NE entries */
    static constexpr size_t NE = N / B;

    /* cache line size */
    static constexpr uint64_t CACHELINE_SIZE = 64;

    /* the number of version bits */
    static constexpr uint64_t VERSION_BITS = 44;

    /* the number of index bits */
    static constexpr uint64_t INDEX_BITS = 20;

    /* make sure parameters are valid */
    static_assert(NE < (1UL << INDEX_BITS), "too many entries in one block");
    static_assert(N % B == 0, "N % B must be 0");

    /* 64 bit Field, contains two segments, version and index */
    struct Field {
        Field() {}
        Field(uint64_t vsn, uint64_t idx) : version(vsn), index(idx) {}
        Field operator+(uint64_t n) {
            index += n;
            return *this;
        } 
        struct {
            uint64_t version : VERSION_BITS;
            uint64_t index : INDEX_BITS;
        };
    };

    struct Block;
    /* Cursor, block metadata for the producer and the consumer */
    struct Cursor {
        Cursor() : field(Field()) {}
        void init(bool first, Block* block) {
            Field f = first ? Field(1, 0) : Field(0, NE);
            bbq_store_rlx(field, f);
            next = block;
            is_first = first;
        }
        alignas(CACHELINE_SIZE) std::atomic<Field> field;
        Block* next;
        bool is_first;
    } __attribute__((aligned(CACHELINE_SIZE)));

    /* block, contains NE entries */
    struct Block {
        Block(){}
        void init(bool is_first, Block* next) {
            prod.init(is_first, next);
            cons.init(is_first, next);
        }
        alignas(CACHELINE_SIZE) Cursor prod;
        alignas(CACHELINE_SIZE) Cursor cons;
        alignas(CACHELINE_SIZE) T data[NE];
        __attribute__((always_inline)) bool prod_ready(uint64_t vsn) {
            Field p = bbq_load_rlx(prod.field);
            return (p.version == vsn);
        }
        __attribute__((always_inline)) bool cons_ready(uint64_t vsn) {
            Field c = bbq_load_acq(cons.field);
            return (c.version == vsn && c.index == NE) || (c.version > vsn);
        }
    } __attribute__((aligned(CACHELINE_SIZE)));

public:
    Queue() {
        head = tail = &blocks[0];
        for (uint64_t i = 0; i < B; i++) {
            blocks[i].init(i == 0, &blocks[(i + 1) % B]);
        }
    }
    __attribute__((always_inline)) bool enqueue(T t) {
    again:;
        Field p = bbq_load_rlx(head->prod.field);
        if bbq_likely (p.index < NE) {
            head->data[p.index] = t;
            bbq_store_rel(head->prod.field, p + 1);
            return true;
        }
        if bbq_likely(prod_advance()) goto again;
        return false;
    }
    __attribute__((always_inline)) bool dequeue(T& t) {
    again:;
        Field c = bbq_load_rlx(tail->cons.field);
        if bbq_likely (c.index < NE) {
            Field p = bbq_load_acq(tail->prod.field);
            if bbq_unlikely (p.index == c.index) return false;
            t = tail->data[c.index];
            bbq_store_rel(tail->cons.field, c + 1);
            return true;
        }
        if bbq_likely(cons_advance()) goto again;
        return false;
    }

private:
    __attribute__((noinline)) bool prod_advance() {
        Block* nb = head->prod.next;
        Field p = bbq_load_rlx(head->prod.field);
        uint64_t nvsn = p.version + nb->prod.is_first;
        if (!nb->cons_ready(nvsn - 1)) return false;
        Field np(nvsn, 0);
        bbq_store_rlx(nb->prod.field, np);
        head = nb;
        return true;
    }
    __attribute__((noinline)) Block* cons_advance() {
        Block* nb = tail->cons.next;
        Field c = bbq_load_rlx(tail->cons.field);
        uint64_t nvsn = c.version + nb->cons.is_first;
        if (!nb->prod_ready(nvsn)) return nullptr;
        Field np(nvsn, 0);
        bbq_store_rlx(nb->cons.field, np);
        tail = nb;
        return nb;
    }

private:
    alignas(CACHELINE_SIZE) Block* head;
    alignas(CACHELINE_SIZE) Block* tail;
    alignas(CACHELINE_SIZE) Block blocks[B];
};

}
}
}