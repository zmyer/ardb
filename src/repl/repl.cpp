#include "repl.hpp"
#include "redis/crc64.h"
#include "db/db.hpp"

#define SERVER_KEY_SIZE 40
#define RUN_PERIOD(name, ms) static uint64_t name##_exec_ms = 0;  \
    if(ms > 0 && (now - name##_exec_ms >= ms) && (name##_exec_ms = now))
OP_NAMESPACE_BEGIN
    static ReplicationService _repl_singleton;
    ReplicationService* g_repl = &_repl_singleton;
    struct ReplMeta
    {
//            uint64_t data_cksm;
//            uint64_t data_offset;  //data offset saved in  store
            char select_ns[ARDB_MAX_NAMESPACE_SIZE];
            char serverkey[SERVER_KEY_SIZE];
            char replkey[SERVER_KEY_SIZE];
            uint16 select_ns_size;
            ReplMeta() :
                    select_ns_size(0)
            {
            }
    };

    ReplicationBacklog::ReplicationBacklog() :
            m_wal(NULL)
    {
    }
    void ReplicationBacklog::Routine()
    {
        uint64_t now = get_current_epoch_millis();
        RUN_PERIOD(sync, g_db->GetConf().repl_backlog_sync_period * 1000)
        {
            FlushSyncWAL();
        }
    }
    int ReplicationBacklog::Init()
    {
        swal_options_t* options = swal_options_create();
        options->create_ifnotexist = true;
        options->user_meta_size = 4096;
        options->max_file_size = g_db->GetConf().repl_backlog_size;
        options->ring_cache_size = g_db->GetConf().repl_backlog_cache_size;
        options->cksm_func = crc64;
        options->log_prefix = "ardb";
        int err = swal_open(g_db->GetConf().repl_data_dir.c_str(), options, &m_wal);
        swal_options_destroy(options);
        if (0 != err)
        {
            ERROR_LOG("Failed to init wal log with err code:%d", err);
            return err;
        }
        ReplMeta* meta = (ReplMeta*) swal_user_meta(m_wal);
        if (meta->serverkey[0] == 0)
        {
            std::string randomkey = random_hex_string(SERVER_KEY_SIZE);
            memcpy(meta->serverkey, randomkey.data(), SERVER_KEY_SIZE);
            memcpy(meta->replkey, randomkey.data(), SERVER_KEY_SIZE);
            memset(meta->select_ns, 0, ARDB_MAX_NAMESPACE_SIZE);
        }
        return 0;
    }
    void ReplicationBacklog::FlushSyncWAL()
    {
        swal_sync(m_wal);
        swal_sync_meta(m_wal);
    }
    swal_t* ReplicationBacklog::GetWAL()
    {
        return m_wal;
    }
    std::string ReplicationBacklog::GetReplKey()
    {
        ReplMeta* meta = (ReplMeta*) swal_user_meta(m_wal);
        std::string str;
        str.assign(meta->replkey, SERVER_KEY_SIZE);
        return str;
    }
    void ReplicationBacklog::SetReplKey(const std::string& str)
    {
        ReplMeta* meta = (ReplMeta*) swal_user_meta(m_wal);
        memcpy(meta->replkey, str.data(), str.size() < SERVER_KEY_SIZE ? str.size() : SERVER_KEY_SIZE);
    }
    int ReplicationBacklog::WriteWAL(const Buffer& cmd)
    {
        swal_append(m_wal, cmd.GetRawReadBuffer(), cmd.ReadableBytes());
        return cmd.ReadableBytes();
    }
    int ReplicationBacklog::WriteWAL(const Data& ns, const Buffer& cmd)
    {
        ReplMeta* meta = (ReplMeta*) swal_user_meta(m_wal);
        int len = 0;
        if (meta->select_ns_size != ns.StringLength() || strncmp(ns.CStr(), meta->select_ns, ns.StringLength()))
        {
            if (g_db->GetConf().master_host.empty())
            {
                Buffer select;
                select.Printf("select %s\r\n", ns.AsString().c_str());
                len += WriteWAL(select);
                memcpy(meta->select_ns, ns.CStr(), ns.StringLength());
                meta->select_ns[ns.StringLength()] = 0;
                meta->select_ns_size = ns.StringLength();
            }
            else
            {
                //slave can NOT generate 'select' itself & never reach here
            }
        }
        len += WriteWAL(cmd);
        return len;
    }

    struct ReplCommand
    {
            Data ns;
            Buffer cmdbuf;
    };

    void ReplicationBacklog::WriteWALCallback(Channel*, void* data)
    {
        ReplCommand* cmd = (ReplCommand*) data;
        g_repl->GetReplLog().WriteWAL(cmd->ns, cmd->cmdbuf);
        DELETE(cmd);
        g_repl->GetMaster().SyncWAL();
    }

    int ReplicationBacklog::WriteWAL(const Data& ns, RedisCommandFrame& cmd)
    {
        if (!g_repl->IsInited())
        {
            return -1;
        }
        ReplCommand* repl_cmd = new ReplCommand;
        repl_cmd->ns = ns;
        const Buffer& raw_protocol = cmd.GetRawProtocolData();
        if (raw_protocol.Readable())
        {
            repl_cmd->cmdbuf.Write(raw_protocol.GetRawReadBuffer(), raw_protocol.ReadableBytes());
        }
        else
        {
            RedisCommandEncoder::Encode(repl_cmd->cmdbuf, cmd);
        }
        g_repl->GetIOService().AsyncIO(0, WriteWALCallback, repl_cmd);
        return 0;
    }

    static int cksm_callback(const void* log, size_t loglen, void* data)
    {
        uint64_t* cksm = (uint64_t*) data;
        *cksm = crc64(*cksm, (const unsigned char *) log, loglen);
        return 0;
    }
    bool ReplicationBacklog::IsValidOffsetCksm(int64_t offset, uint64_t cksm)
    {
        bool valid_offset = offset > 0 && offset <= swal_end_offset(m_wal) && offset >= swal_start_offset(m_wal);
        if (!valid_offset)
        {
            return false;
        }
        if (0 == cksm)
        {
            //DO not check cksm when it's 0
            return true;
        }
        uint64_t dest_cksm = swal_cksm(m_wal);
        uint64_t end_offset = swal_end_offset(m_wal);
        swal_replay(m_wal, offset, end_offset - offset, cksm_callback, &cksm);
        return cksm == dest_cksm;
    }
    std::string ReplicationBacklog::CurrentNamespace()
    {
        ReplMeta* meta = (ReplMeta*) swal_user_meta(m_wal);
        std::string str;
        if(meta->select_ns_size > 0)
        {
            str.assign(meta->select_ns, meta->select_ns_size);
        }
        return str;
    }
    uint64_t ReplicationBacklog::WALCksm()
    {
        return swal_cksm(m_wal);
    }
    void ReplicationBacklog::ResetWALOffsetCksm(uint64_t offset, uint64_t cksm)
    {
        swal_reset(m_wal, offset, cksm);
    }
    uint64_t ReplicationBacklog::WALStartOffset()
    {
        return swal_start_offset(m_wal);
    }
    uint64_t ReplicationBacklog::WALEndOffset()
    {
        return swal_end_offset(m_wal);
    }
    ReplicationBacklog::~ReplicationBacklog()
    {
    }

    ReplicationService::ReplicationService() :
            m_inited(false)
    {
        g_repl = this;
    }
    ChannelService& ReplicationService::GetIOService()
    {
        return m_io_serv;
    }
    ReplicationBacklog& ReplicationService::GetReplLog()
    {
        return m_repl_backlog;
    }
    Master& ReplicationService::GetMaster()
    {
        return m_master;
    }
    Slave& ReplicationService::GetSlave()
    {
        return m_slave;
    }
    void ReplicationService::Run()
    {
        struct RoutineTask: public Runnable
        {
                void Run()
                {
                    g_repl->GetMaster().Routine();
                    g_repl->GetSlave().Routine();
                    g_repl->GetReplLog().Routine();
                }
        };
        m_io_serv.GetTimer().Schedule(new RoutineTask, 1, 1, SECONDS);
        m_inited = true;
        m_io_serv.Start();
    }
    bool ReplicationService::IsInited() const
    {
        return m_inited;
    }
    int ReplicationService::Init()
    {
        if (IsInited())
        {
            return 0;
        }
        int err = m_repl_backlog.Init();
        if (0 != err)
        {
            return err;
        }
        err = m_master.Init();
        if (0 != err)
        {
            return err;
        }
        err = m_slave.Init();
        if (0 != err)
        {
            return err;
        }
        Start();
        while (!IsInited())
        {
            usleep(100);
        }
        return 0;
    }
OP_NAMESPACE_END
