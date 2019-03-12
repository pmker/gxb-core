#include <graphene/query_txid/query_txid_plugin.hpp>
#include <fc/io/fstream.hpp>
#include <graphene/chain/transaction_entry_object.hpp>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <leveldb/db.h>
#include <leveldb/write_batch.h>

namespace graphene{ namespace query_txid{

namespace detail
{
    using namespace leveldb;
    class query_txid_plugin_impl
    {   
    public:
        query_txid_plugin_impl(query_txid_plugin& _plugin):_self(_plugin){}
        ~query_txid_plugin_impl(){
            insert_db_thread.join();
        }

        void collect_txid_index(const signed_block &b);

        graphene::chain::database& database()
        {
            return _self.database();
        }
        void init();
    private:
        query_txid_plugin&              _self;
        std::queue<trx_entry_object>    trx_queue;              //read and write queue
        uint64_t                        limit_batch = 1;     //limit of leveldb batch
        uint64_t                        size_queue  = 10000;    //queue size
        std::mutex                      mut;                    //lock
        std::thread                     insert_db_thread;
        std::condition_variable         sig_able_put;               
        std::condition_variable         sig_able_get;           
        uint32_t                        curr_block_num = 0;   

        DB                              *db;
        std::string                     db_path;
        void get_entry_queue();                     //Consume specified number (default 1000) of the entry structure, inserted into leveldb, transactional (insert db and delete from memory to synchronize)
        void put_entry_queue(uint64_t irr_num);     //Produce the entry structure of the confirmed transaction into the queue
    };
    void query_txid_plugin_impl::init(){
        //Create a consumer thread that continuously reads the entry structure from the queue
        insert_db_thread = std::thread(&query_txid_plugin_impl::get_entry_queue,this);
        
        //Create leveldb
        Options options;
        options.create_if_missing = true;
        Status s = DB::Open(options,db_path,&db);

        leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            ilog("key:${key}--value:${value}",("key",it->key().ToString())("value",it->value().ToString()));
        }
        assert(it->status().ok());  // Check for any errors found during the scan
        delete it;
    }
    void query_txid_plugin_impl::collect_txid_index(const signed_block &b){
        graphene::chain::database& db = database();
        for( auto idx = 0; idx < b.transactions.size(); idx++ ){
            db.create<trx_entry_object>( [&]( trx_entry_object& obj )
            {
                obj.txid          = b.transactions[idx].id();
                obj.block_num     = b.block_num();
                obj.trx_in_block  = idx;
            }); 
        }
        // Store irreversible transactions in leveldb according to the current irreversible block
        const auto& dpo = db.get_dynamic_global_properties();
        auto irr_num = dpo.last_irreversible_block_num;
        ilog("irr_number: ${irr_num}", ("irr_num",irr_num));
        put_entry_queue(irr_num);
    }
    void query_txid_plugin_impl::get_entry_queue(){
        std::unique_lock<std::mutex> lock(mut);
        while(trx_queue.size() == 0){
            sig_able_get.wait(lock);    
        }
        while(trx_queue.size()>limit_batch){
            // 从队列中取出前n项元素，插入到leveldb中，使用同步的方式，原子操作，保证插入成功或者失败
            leveldb::WriteBatch batch;
            for(auto idx = 0; idx<limit_batch ; idx++){
                auto item = trx_queue.front();
                trx_queue.pop();
                auto serialize = fc::raw::pack(item);
                std::string txid(item.txid);
                batch.Put(txid,{serialize.data(),serialize.size()});
            }
            leveldb::WriteOptions write_options;
            write_options.sync = true;
            Status s = db->Write(write_options,&batch);
            sig_able_put.notify_all();
        }  
        sig_able_put.notify_all();
        lock.unlock();
    }
    void query_txid_plugin_impl::put_entry_queue(uint64_t irr_num){
        ilog("produce thread todo test11111112 ");
        //1.Write to the queue, use the mutex to lock the queue
        std::unique_lock<std::mutex> lock(mut);
        //2.Push the entry structure of the confirmed transaction into the queue
        while(trx_queue.size() >= size_queue){
            ilog("produce thread todo test ");
            sig_able_put.wait(lock);
        }
        ilog("produce thread todo test222222 ");
        graphene::chain::database& db = database();
        const auto& trxen_idx = db.get_index_type<trx_entry_index>();
        const auto& bybn_idx = trxen_idx.indices().get<by_blocknum>();
        for(auto itor = bybn_idx.lower_bound(curr_block_num); itor != bybn_idx.end(); itor++){
            ilog("block_num: ${block_num}",("block_num",itor->block_num));
            if(itor->block_num < irr_num){
                trx_queue.push(*itor);
                curr_block_num = itor->block_num;
                sig_able_get.notify_all();
                ilog("curr_block: ${curr_block}",("curr_block",curr_block_num));
            }else{ break;}   
        }   
        curr_block_num++;
        //3.Send a signal to release the mutex
        lock.unlock();
    }
}


// -----------------------------------query_txid_plugin --------------------------------------

query_txid_plugin::query_txid_plugin():
    my(new detail::query_txid_plugin_impl(*this)){
}

query_txid_plugin::~query_txid_plugin(){
    
}

std::string query_txid_plugin::plugin_name()const {
    return "query_txid";
}

void query_txid_plugin::plugin_set_program_options(
    boost::program_options::options_description& cli,
            boost::program_options::options_description& cfg){

}

void query_txid_plugin::plugin_initialize(const boost::program_options::variables_map& options){
    ilog("test leveldb plugin!!!!");
    // Add the index of the trx_entry_index object table to the database
    database().add_index< primary_index< trx_entry_index > >();
    // Respond to the apply_block signal
    database().applied_block.connect( [&]( const signed_block& b){ my->collect_txid_index(b); } );
    // Initialize the plugin instance
    my->init();
}

void query_txid_plugin::plugin_startup(){

}

}}