#include <boost/algorithm/string.hpp>
#include <map>
#include <vector>
#include "configdb.h"
#include "pubsub.h"
#include "converter.h"

using namespace std;
using namespace swss;

ConfigDBConnector::ConfigDBConnector(bool use_unix_socket_path, const char *netns)
    : SonicV2Connector(use_unix_socket_path, netns)
    , TABLE_NAME_SEPARATOR("|")
    , KEY_SEPARATOR("|")
{
}

void ConfigDBConnector::db_connect(string db_name, bool wait_for_init, bool retry_on)
{
    m_db_name = db_name;
    KEY_SEPARATOR = TABLE_NAME_SEPARATOR = get_db_separator(db_name);
    SonicV2Connector::connect(m_db_name, retry_on);

    if (wait_for_init)
    {
        auto& client = get_redis_client(m_db_name);
        auto pubsub = client.pubsub();
        auto initialized = client.get(INIT_INDICATOR);
        if (!initialized || initialized->empty())
        {
            string pattern = "__keyspace@" + to_string(get_dbid(m_db_name)) +  "__:" + INIT_INDICATOR;
            pubsub->psubscribe(pattern);
            for (;;)
            {
                auto item = pubsub->listen_message();
                if (item["type"] == "pmessage")
                {
                    string channel = item["channel"];
                    size_t pos = channel.find(':');
                    string key;
                    if (pos != string::npos)
                    {
                        key = channel.substr(pos + 1);
                    }
                    if (key == INIT_INDICATOR)
                    {
                        initialized = client.get(INIT_INDICATOR);
                        if (initialized && !initialized->empty())
                        {
                            break;
                        }
                    }
                }
            }
            pubsub->punsubscribe(pattern);
        }
    }
}

void ConfigDBConnector::connect(bool wait_for_init, bool retry_on)
{
    db_connect("CONFIG_DB", wait_for_init, retry_on);
}

// Write a table entry to config db.
//    Remove extra fields in the db which are not in the data.
// Args:
//     table: Table name.
//     key: Key of table entry, or a tuple of keys if it is a multi-key table.
//     data: Table row data in a form of dictionary {'column_key': 'value', ...}.
//           Pass {} as data will delete the entry.
void ConfigDBConnector::set_entry(string table, string key, const map<string, string>& data)
{
    auto& client = get_redis_client(m_db_name);
    string _hash = to_upper(table) + TABLE_NAME_SEPARATOR + key;
    if (data.empty())
    {
        client.del(_hash);
    }
    else
    {
        auto original = get_entry(table, key);
        client.hmset(_hash, data.begin(), data.end());
        for (auto& it: original)
        {
            auto& k = it.first;
            bool found = data.find(k) == data.end();
            if (!found)
            {
                client.hdel(_hash, k);
            }
        }
    }
}

// Modify a table entry to config db.
// Args:
//     table: Table name.
//     key: Key of table entry, or a tuple of keys if it is a multi-key table.
//     data: Table row data in a form of dictionary {'column_key': 'value', ...}.
//           Pass {} as data will create an entry with no column if not already existed.
//           Pass None as data will delete the entry.
void ConfigDBConnector::mod_entry(string table, string key, const map<string, string>& data)
{
    auto& client = get_redis_client(m_db_name);
    string _hash = to_upper(table) + TABLE_NAME_SEPARATOR + key;
    if (data.empty())
    {
        client.del(_hash);
    }
    else
    {
        client.hmset(_hash, data.begin(), data.end());
    }
}

// Read a table entry from config db.
// Args:
//     table: Table name.
//     key: Key of table entry, or a tuple of keys if it is a multi-key table.
// Returns:
//     Table row data in a form of dictionary {'column_key': 'value', ...}
//     Empty dictionary if table does not exist or entry does not exist.
map<string, string> ConfigDBConnector::get_entry(string table, string key)
{
    auto& client = get_redis_client(m_db_name);
    string _hash = to_upper(table) + TABLE_NAME_SEPARATOR + key;
    return client.hgetall(_hash);
}

// Read all keys of a table from config db.
// Args:
//     table: Table name.
//     split: split the first part and return second.
//            Useful for keys with two parts <tablename>:<key>
// Returns:
//     List of keys.
vector<string> ConfigDBConnector::get_keys(string table, bool split)
{
    auto& client = get_redis_client(m_db_name);
    string pattern = to_upper(table) + TABLE_NAME_SEPARATOR + "*";
    const auto& keys = client.keys(pattern);
    vector<string> data;
    for (auto& key: keys)
    {
        if (split)
        {
            size_t pos = key.find(TABLE_NAME_SEPARATOR);
            string row;
            if (pos != string::npos)
            {
                row = key.substr(pos + 1);
            }
            data.push_back(row);
        }
        else
        {
            data.push_back(key);
        }
    }
    return data;
}

// Read an entire table from config db.
// Args:
//     table: Table name.
// Returns:
//     Table data in a dictionary form of
//     { 'row_key': {'column_key': value, ...}, ...}
//     or { ('l1_key', 'l2_key', ...): {'column_key': value, ...}, ...} for a multi-key table.
//     Empty dictionary if table does not exist.
map<string, map<string, string>> ConfigDBConnector::get_table(string table)
{
    auto& client = get_redis_client(m_db_name);
    string pattern = to_upper(table) + TABLE_NAME_SEPARATOR + "*";
    const auto& keys = client.keys(pattern);
    map<string, map<string, string>> data;
    for (auto& key: keys)
    {
        auto const& entry = client.hgetall(key);
        size_t pos = key.find(TABLE_NAME_SEPARATOR);
        string row;
        if (pos != string::npos)
        {
            row = key.substr(pos + 1);
        }
        data[row] = entry;
    }
    return data;
}

// Delete an entire table from config db.
// Args:
//     table: Table name.
void ConfigDBConnector::delete_table(string table)
{
    auto& client = get_redis_client(m_db_name);
    string pattern = to_upper(table) + TABLE_NAME_SEPARATOR + "*";
    const auto& keys = client.keys(pattern);
    for (auto& key: keys)
    {
        client.del(key);
    }
}

// Write multiple tables into config db.
//    Extra entries/fields in the db which are not in the data are kept.
// Args:
//     data: config data in a dictionary form
//     {
//         'TABLE_NAME': { 'row_key': {'column_key': 'value', ...}, ...},
//         'MULTI_KEY_TABLE_NAME': { ('l1_key', 'l2_key', ...) : {'column_key': 'value', ...}, ...},
//         ...
//     }
void ConfigDBConnector::mod_config(const map<string, map<string, map<string, string>>>& data)
{
    for (auto const& it: data)
    {
        string table_name = it.first;
        auto const& table_data = it.second;
        if (table_data.empty())
        {
            delete_table(table_name);
            continue;
        }
        for (auto const& ie: table_data)
        {
            string key = ie.first;
            auto const& fvs = ie.second;
            mod_entry(table_name, key, fvs);
        }
    }
}

// Read all config data.
// Returns:
//     Config data in a dictionary form of
//     {
//         'TABLE_NAME': { 'row_key': {'column_key': 'value', ...}, ...},
//         'MULTI_KEY_TABLE_NAME': { ('l1_key', 'l2_key', ...) : {'column_key': 'value', ...}, ...},
//         ...
//     }
map<string, map<string, map<string, string>>> ConfigDBConnector::get_config()
{
    auto& client = get_redis_client(m_db_name);
    auto const& keys = client.keys("*");
    map<string, map<string, map<string, string>>> data;
    for (string key: keys)
    {
        size_t pos = key.find(TABLE_NAME_SEPARATOR);
        if (pos == string::npos)
        {
            continue;
        }
        string table_name = key.substr(0, pos);
        string row = key.substr(pos + 1);
        auto const& entry = client.hgetall(key);

        if (!entry.empty())
        {
            data[table_name][row] = entry;
        }
    }
    return data;
}

std::string ConfigDBConnector::getKeySeparator() const
{
    return KEY_SEPARATOR;
}

ConfigDBPipeConnector::ConfigDBPipeConnector(bool use_unix_socket_path, const char *netns)
    : ConfigDBConnector(use_unix_socket_path, netns)
{
}

// Helper method to delete table entries from config db using Redis pipeline
// with batch size of REDIS_SCAN_BATCH_SIZE.
// The caller should call pipeline execute once ready
// Args:
//     client: Redis client
//     pipe: Redis DB pipe
//     pattern: key pattern
//     cursor: position to start scanning from
//
// Returns:
//     cur: poition of next item to scan
int64_t ConfigDBPipeConnector::_delete_entries(DBConnector& client, RedisTransactioner& pipe, const char *pattern, int64_t cursor)
{
    const auto& rc = client.scan(cursor, pattern, REDIS_SCAN_BATCH_SIZE);
    auto cur = rc.first;
    auto& keys = rc.second;
    for (auto const& key: keys)
    {
        RedisCommand sdel;
        sdel.format("DEL %s", key.c_str());
        pipe.enqueue(sdel.c_str(), REDIS_REPLY_INTEGER);
    }

    return cur;
}

// Helper method to delete table entries from config db using Redis pipeline.
// The caller should call pipeline execute once ready
// Args:
//     client: Redis client
//     pipe: Redis DB pipe
//     table: Table name.
void ConfigDBPipeConnector::_delete_table(DBConnector& client, RedisTransactioner& pipe, string table)
{
    string pattern = to_upper(table) + TABLE_NAME_SEPARATOR + "*";
    auto cur = _delete_entries(client, pipe, pattern.c_str(), 0);
    while (cur != 0)
    {
        cur = _delete_entries(client, pipe, pattern.c_str(), cur);
    }
}

// Modify a table entry to config db.
// Args:
//     table: Table name.
//     pipe: Redis DB pipe
//     table: Table name.
//     key: Key of table entry, or a tuple of keys if it is a multi-key table.
//     data: Table row data in a form of dictionary {'column_key': 'value', ...}.
//           Pass {} as data will create an entry with no column if not already existed.
//           Pass None as data will delete the entry.
void ConfigDBPipeConnector::_mod_entry(RedisTransactioner& pipe, string table, string key, const map<string, string>& data)
{
    string _hash = to_upper(table) + TABLE_NAME_SEPARATOR + key;
    if (data.empty())
    {
        RedisCommand sdel;
        sdel.format("DEL %s", _hash.c_str());
        pipe.enqueue(sdel.c_str(), REDIS_REPLY_INTEGER);
    }
    else
    {
        RedisCommand shmset;
        shmset.formatHMSET(_hash, data.begin(), data.end());
        pipe.enqueue(shmset.c_str(), REDIS_REPLY_STATUS);
    }
}
// Write multiple tables into config db.
//    Extra entries/fields in the db which are not in the data are kept.
// Args:
//     data: config data in a dictionary form
//     {
//         'TABLE_NAME': { 'row_key': {'column_key': 'value', ...}, ...},
//         'MULTI_KEY_TABLE_NAME': { ('l1_key', 'l2_key', ...) : {'column_key': 'value', ...}, ...},
//         ...
//     }
void ConfigDBPipeConnector::mod_config(const map<string, map<string, map<string, string>>>& data)
{
    auto& client = get_redis_client(m_db_name);
    RedisTransactioner pipe(&client);
    pipe.multi();
    for (auto const& id: data)
    {
        auto& table_name = id.first;
        auto& table_data = id.second;
        if (table_data.empty())
        {
            _delete_table(client, pipe, table_name);
            continue;
        }
        for (auto const& it: table_data)
        {
            auto& key = it.first;
            _mod_entry(pipe, table_name, key, it.second);
        }
    }
    pipe.exec();
}

// Read config data in batches of size REDIS_SCAN_BATCH_SIZE using Redis pipelines
// Args:
//     client: Redis client
//     pipe: Redis DB pipe
//     data: config dictionary
//     cursor: position to start scanning from
//
// Returns:
//     cur: poition of next item to scan
int64_t ConfigDBPipeConnector::_get_config(DBConnector& client, RedisTransactioner& pipe, map<string, map<string, map<string, string>>>& data, int64_t cursor)
{
    auto const& rc = client.scan(cursor, "*", REDIS_SCAN_BATCH_SIZE);
    auto cur = rc.first;
    auto const& keys = rc.second;
    pipe.multi();
    for (auto const& key: keys)
    {
        if (key == INIT_INDICATOR)
        {
            continue;
        }
        RedisCommand shgetall;
        shgetall.format("HGETALL %s", key.c_str());
        pipe.enqueue(shgetall.c_str(), REDIS_REPLY_ARRAY);
    }
    pipe.exec();

    for (auto const& key: keys)
    {
        if (key == INIT_INDICATOR)
        {
            continue;
        }

        size_t pos = key.find(TABLE_NAME_SEPARATOR);
        if (pos == string::npos)
        {
            continue;
        }
        string table_name = key.substr(0, pos - 1);
        string row = key.substr(pos + 1);

        auto reply = pipe.dequeueReply();
        if (reply == NULL)
        {
            continue;
        }
        RedisReply r(reply);

        auto dataentry = data[table_name][row];
        for (unsigned int i = 0; i < r.getChildCount(); i += 2)
        {
            string field = r.getChild(i)->str;
            string value = r.getChild(i+1)->str;
            dataentry.emplace(field, value);
        }
    }
    return cur;
}

map<string, map<string, map<string, string>>> ConfigDBPipeConnector::get_config()
{
    auto& client = get_redis_client(m_db_name);
    RedisTransactioner pipe(&client);

    map<string, map<string, map<string, string>>> data;
    auto cur = _get_config(client, pipe, data, 0);
    while (cur != 0)
    {
        cur = _get_config(client, pipe, data, cur);
    }

    return data;
}
