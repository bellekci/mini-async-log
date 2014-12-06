/*
 * memory_mpmc_bounded.hpp
 *
 *  Created on: Dec 6, 2014
 *      Author: rafa
 */

#ifndef UFO_LOG_MEMORY_MPMC_BOUNDED_HPP_
#define UFO_LOG_MEMORY_MPMC_BOUNDED_HPP_

#include <cassert>
#include <new>
#include <ufo_log/util/atomic.hpp>

namespace ufo {

// This is the Djukov MPMC queue that adds the trivial conversion to single
// producer or single consumer funcions so it can be used as a mpmc, spmc or
// mpsc. Of course you can't mix two different producer modes on the same queue.
//
// It is just tought to suit my needs by breaking the push and pop function in
// two parts or with order wording, made half intrusive.
//
//--------------------------------------------------------------------------
class memory_mpmc_b_fifo_entry
{
public:
    //----------------------------------------------------------------------
    transaction_data()
    {
        cell = mem = nullptr;
        size = pos = 0;
    }
    //----------------------------------------------------------------------
    u8*    mem;
    size_t size;
    //----------------------------------------------------------------------
private:
    //----------------------------------------------------------------------
    friend class memory_mpmc_b_fifo;
    void*  cell;
    size_t pos;
    //----------------------------------------------------------------------
};
//------------------------------------------------------------------------------
class memory_mpmc_b_fifo
{
public:
    //--------------------------------------------------------------------------
    memory_mpmc_b_fifo()
    {
        m_buffer      = nullptr;
        m_mem         = nullptr;
        m_buffer_mask = 0;
        m_entry_size  = 0;
        m_enqueue_pos = 0;
        m_dequeue_pos = 0;
    }
    //--------------------------------------------------------------------------
    ~memory_mpmc_b_fifo()
    {
        clear();
    }
    //--------------------------------------------------------------------------
    void clear()                                                                //Dangerous, just to be used after failed initializations
    {
        if (m_buffer)
        {
            delete [] m_buffer;
        }
        if (m_mem)
        {
            ::operator delete (m_mem);
        }
    }
    //--------------------------------------------------------------------------
    bool init (size_t total_bytes, size_t entries)
    {
        if ((entries >= 2) &&
            ((entries & (entries - 1)) == 0) &&
            (total_bytes >= entries) &&
            !initialized()
            )
        {
            clear();
            auto real_bytes = (total_bytes / entries) * entries;

            m_entry_size  = real_bytes / entries;
            m_enqueue_pos = 0;
            m_dequeue_pos = 0;
            m_buffer_mask = entries - 1;

            m_buffer = new (std::nothrow) cell_t [entries];
            if (!m_buffer) { return false; }

            m_mem = (u8*) ::operator new (real_bytes, std::nothrow);
            if (!m_mem) { return false; }

            for (size_t i = 0; i != entries; i += 1)
            {
                m_buffer[i].sequence = i;
                m_buffer[i].mem      = m_mem + (i * m_entry_size);
            }
            return true;
        }
        return false;
    }
    //--------------------------------------------------------------------------
    bool initialized() const
    {
        return m_buffer && m_mem;
    }
    //--------------------------------------------------------------------------
    size_t entry_size() const { return m_entry_size; }
    //--------------------------------------------------------------------------
    memory_mpmc_b_fifo_entry acquire_mp_bounded_push_data()
    {
        assert (m_buffer);
        memory_mpmc_b_fifo_entry e;
        cell_t* cell;
        size_t pos = m_enqueue_pos;
        for (;;)
        {
            cell          = &m_buffer[pos & m_buffer_mask];
            size_t seq    = cell->sequence.load (mo_acquire);
            intptr_t diff = (intptr_t) seq - (intptr_t) pos;
            if (diff == 0)
            {
                if (m_enqueue_pos.compare_exchange_weak(
                    pos, pos + 1, mo_relaxed
                    ))
                {
                    break;
                }
            }
            else if (diff < 0)
            {
                return e;
            }
            else
            {
                pos = m_enqueue_pos;
            }
        }
        e.cell = cell;
        e.pos  = pos + 1;
        e.mem  = cell->mem;
        e.size = entry_size();
        return e;
    }
    //--------------------------------------------------------------------------
    memory_mpmc_b_fifo_entry acquire_sp_bounded_push_data()
    {
        assert (m_buffer);
        memory_mpmc_b_fifo_entry e;
        cell_t* cell  = &m_buffer[m_enqueue_pos & m_buffer_mask];
        size_t seq    = cell->sequence.load (mo_acquire);
        intptr_t diff = (intptr_t) seq - (intptr_t) m_enqueue_pos;
        if (diff == 0)
        {
            ++m_enqueue_pos;
            e.cell = cell;
            e.pos  = m_enqueue_pos;
            e.mem  = cell->mem;
            e.size = entry_size();
            return e;
        }
        assert (diff < 0);
        return e;
    }
    //--------------------------------------------------------------------------
    void do_push (const memory_mpmc_b_fifo_entry& e)
    {
        if (e.cell)
        {
            ((cell_t*) e.cell)->sequence.store (e.pos, mo_release);
        }
    }
    //--------------------------------------------------------------------------
    bool acquire_mc_pop_data()
    {
        assert (m_buffer);
        memory_mpmc_b_fifo_entry e;
        cell_t* cell;
        size_t pos = m_dequeue_pos;
        for (;;)
        {
            cell          = &m_buffer[pos & m_buffer_mask];
            size_t seq    = cell->sequence.load (mo_acquire);
            intptr_t diff = (intptr_t) seq - (intptr_t) (pos + 1);
            if (diff == 0)
            {
                if (m_dequeue_pos.compare_exchange_weak(
                    pos, pos + 1, mo_relaxed
                    ))
                {
                    break;
                }
            }
            else if (diff < 0)
            {
                return e;
            }
            else
            {
                pos = m_dequeue_pos;
            }
        }
        e.cell = cell;
        e.pos  = pos + m_buffer_mask + 1;
        e.mem  = cell->mem;
        e.size = entry_size();
        return e;
    }
    //--------------------------------------------------------------------------
    bool acquire_sc_pop_data()
    {
        assert (m_buffer);
        memory_mpmc_b_fifo_entry e;
        cell_t* cell  = &m_buffer[m_dequeue_pos & m_buffer_mask];
        size_t seq    = cell->sequence.load (mo_acquire);
        intptr_t diff = (intptr_t) seq - (intptr_t) (m_dequeue_pos + 1);
        if (diff == 0)
        {
            ++m_dequeue_pos;
            e.cell = cell;
            e.pos  = m_dequeue_pos + m_buffer_mask;
            e.mem  = cell->mem;
            e.size = entry_size();
            return e;
        }
        assert (diff < 0);
        return e;
    }
    //--------------------------------------------------------------------------
    void release_pop (const memory_mpmc_b_fifo_entry& e)
    {
        if (e.cell)
        {
            ((cell_t*) e.cell)->sequence.store (e.pos, mo_release);
        }
    }
    //--------------------------------------------------------------------------
private:
    //--------------------------------------------------------------------------
    struct cell_t
    {
        at::atomic<size_t> sequence;
        void*              mem;
    };
    //--------------------------------------------------------------------------
    typedef char cacheline_pad_t [cache_line_size];

    cacheline_pad_t           m_pad0;

    cell_t*                   m_buffer;
    size_t const              m_buffer_mask;
    size_t const              m_entry_size;
    u8*                       m_mem;

    cacheline_pad_t           m_pad1;

    mo_relaxed_atomic<size_t> m_enqueue_pos;

    cacheline_pad_t           m_pad2;

    mo_relaxed_atomic<size_t> m_dequeue_pos;

    cacheline_pad_t           m_pad3;

    mpmc_b_fifo (mpmc_b_fifo const&);
    void operator= (mpmc_b_fifo const&);
};
//------------------------------------------------------------------------------
} //namespaces

#endif /* UFO_LOG_MEMORY_MPMC_BOUNDED_HPP_ */
