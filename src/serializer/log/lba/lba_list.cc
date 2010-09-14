
#include <vector>
#include "utils.hpp"
#include "lba_list.hpp"
#include "cpu_context.hpp"
#include "event_queue.hpp"

lba_list_t::lba_list_t(extent_manager_t *em)
    : extent_manager(em), state(state_unstarted), in_memory_index(NULL), disk_structure(NULL)
    {}

/* This form of start() is called when we are creating a new database */
void lba_list_t::start(fd_t fd) {
    
    assert(state == state_unstarted);
    
    dbfd = fd;
    
    in_memory_index = new in_memory_index_t();
    
    lba_disk_structure_t::create(extent_manager, dbfd, &disk_structure);
    
    state = state_ready;
}

struct lba_start_fsm_t :
    private lba_disk_structure_t::load_callback_t,
    public alloc_mixin_t<tls_small_obj_alloc_accessor<alloc_t>, lba_start_fsm_t>
{
    enum {
        state_start,
        state_loading_lba,
        state_done
    } state;
    
    lba_list_t *owner;
    
    lba_list_t::ready_callback_t *callback;
    
    lba_start_fsm_t(lba_list_t *l)
        : state(state_start), owner(l)
        {}
    
    ~lba_start_fsm_t() {
        assert(state == state_start || state == state_done);
    }
    
    bool run(lba_list_t::metablock_mixin_t *last_metablock, lba_list_t::ready_callback_t *cb) {
        
        assert(state == state_start);
        
        assert(owner->state == lba_list_t::state_unstarted);
        owner->state = lba_list_t::state_starting_up;
        
        bool done = lba_disk_structure_t::load(owner->extent_manager, owner->dbfd, last_metablock,
            &owner->disk_structure, this);
        
        if (done) {
            callback = NULL;
            finish();
            return true;
        } else {
            callback = cb;
            return false;
        }
    }
    
    void on_load_lba() {
        finish();
    }
    
    void finish() {
        owner->in_memory_index = new in_memory_index_t(owner->disk_structure);
        owner->state = lba_list_t::state_ready;
        
        if (callback) callback->on_lba_ready();
        delete this;
    }
};

/* This form of start() is called when we are loading an existing database */
bool lba_list_t::start(fd_t fd, metablock_mixin_t *last_metablock, ready_callback_t *cb) {
    
    assert(state == state_unstarted);
    
    dbfd = fd;
    
    lba_start_fsm_t *starter = new lba_start_fsm_t(this);
    return starter->run(last_metablock, cb);
}

block_id_t lba_list_t::gen_block_id() {
    assert(state == state_ready);
    
    return in_memory_index->gen_block_id();
}

off64_t lba_list_t::get_block_offset(block_id_t block) {
    assert(state == state_ready);
    
    return in_memory_index->get_block_offset(block);
}

struct lba_changer_t :
    public lba_disk_structure_t::sync_callback_t,
    public alloc_mixin_t<tls_small_obj_alloc_accessor<alloc_t>, lba_changer_t>
{
    
    lba_list_t *owner;
    lba_changer_t(lba_list_t *owner) : owner(owner) {}
    
    lba_entry_t *entries;
    int nentries;
    lba_list_t::sync_callback_t *callback;
    
    bool run(lba_entry_t *_entries, int _nentries, lba_list_t::metablock_mixin_t *mb_out, lba_list_t::sync_callback_t *cb) {
        
        entries = _entries;
        nentries = _nentries;
        
        bool done = owner->disk_structure->write(entries, nentries, mb_out, this);
        
        if (done) {
            callback = NULL;
            finish();
            return true;
        } else {
            callback = cb;
            return false;
        }
    }
    
    void on_sync_lba() {
        finish();
    }
    
    void finish() {
        
        /* TODO: We should update the in-memory index after the metablock finishes writing,
        immediately before the serializer calls back the client to say that the transaction is
        finished. */
        
        for (int i = 0; i < nentries; i ++) {
            if (entries[i].offset == DELETE_BLOCK) {
                owner->in_memory_index->delete_block(entries[i].block_id);
            } else {
                owner->in_memory_index->set_block_offset(entries[i].block_id, entries[i].offset);
            }
        }
        
        if (callback) callback->on_lba_sync();
        
        delete this;
    }
};

bool lba_list_t::write(entry_t *entries, int nentries, metablock_mixin_t *mb_out, sync_callback_t *cb) {
    assert(state == state_ready);
    
    lba_changer_t *changer = new lba_changer_t(this);
    return changer->run(entries, nentries, mb_out, cb);
}

void lba_list_t::shutdown() {

    assert(state == state_ready);
    
    delete in_memory_index;
    in_memory_index = NULL;
    
    disk_structure->shutdown();   // Also deletes it
    disk_structure = NULL;
    
    state = state_shut_down;
}

lba_list_t::~lba_list_t() {
    assert(state == state_unstarted || state == state_shut_down);
    assert(in_memory_index == NULL);
    assert(disk_structure == NULL);
}

