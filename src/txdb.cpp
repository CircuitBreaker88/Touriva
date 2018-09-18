// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdb.h"

#include "chainparams.h"
#include "hash.h"
#include "main.h"
#include "pow.h"
#include "uint256.h"
#include "core_io.h"

#include <stdint.h>

#include <boost/thread.hpp>

using namespace std;

// NOTE: Per issue #3277, do not use the prefix 'X' or 'x' as they were
// previously used by DB_SAPLING_ANCHOR and DB_BEST_SAPLING_ANCHOR.
static const char DB_SPROUT_ANCHOR = 'A';
static const char DB_SAPLING_ANCHOR = 'Z';
static const char DB_NULLIFIER = 's';
static const char DB_SAPLING_NULLIFIER = 'S';
static const char DB_COINS = 'c';
static const char DB_BLOCK_FILES = 'f';
static const char DB_TXINDEX = 't';
static const char DB_ADDRESSINDEX = 'd';
static const char DB_ADDRESSUNSPENTINDEX = 'u';
static const char DB_TIMESTAMPINDEX = 'S';
static const char DB_BLOCKHASHINDEX = 'z';
static const char DB_SPENTINDEX = 'p';
static const char DB_BLOCK_INDEX = 'b';

static const char DB_BEST_BLOCK = 'B';
static const char DB_BEST_SPROUT_ANCHOR = 'a';
static const char DB_BEST_SAPLING_ANCHOR = 'z';
static const char DB_FLAG = 'F';
static const char DB_REINDEX_FLAG = 'R';
static const char DB_LAST_BLOCK = 'l';


CCoinsViewDB::CCoinsViewDB(std::string dbName, size_t nCacheSize, bool fMemory, bool fWipe) : db(GetDataDir() / dbName, nCacheSize, fMemory, fWipe) {
}

CCoinsViewDB::CCoinsViewDB(size_t nCacheSize, bool fMemory, bool fWipe) : db(GetDataDir() / "chainstate", nCacheSize, fMemory, fWipe) 
{
}


bool CCoinsViewDB::GetSproutAnchorAt(const uint256 &rt, SproutMerkleTree &tree) const {
    if (rt == SproutMerkleTree::empty_root()) {
        SproutMerkleTree new_tree;
        tree = new_tree;
        return true;
    }

    bool read = db.Read(make_pair(DB_SPROUT_ANCHOR, rt), tree);

    return read;
}

bool CCoinsViewDB::GetSaplingAnchorAt(const uint256 &rt, SaplingMerkleTree &tree) const {
    if (rt == SaplingMerkleTree::empty_root()) {
        SaplingMerkleTree new_tree;
        tree = new_tree;
        return true;
    }

    bool read = db.Read(make_pair(DB_SAPLING_ANCHOR, rt), tree);

    return read;
}

bool CCoinsViewDB::GetNullifier(const uint256 &nf, ShieldedType type) const {
    bool spent = false;
    char dbChar;
    switch (type) {
        case SPROUT:
            dbChar = DB_NULLIFIER;
            break;
        case SAPLING:
            dbChar = DB_SAPLING_NULLIFIER;
            break;
        default:
            throw runtime_error("Unknown shielded type");
    }
    return db.Read(make_pair(dbChar, nf), spent);
}

bool CCoinsViewDB::GetCoins(const uint256 &txid, CCoins &coins) const {
    return db.Read(make_pair(DB_COINS, txid), coins);
}

bool CCoinsViewDB::HaveCoins(const uint256 &txid) const {
    return db.Exists(make_pair(DB_COINS, txid));
}

uint256 CCoinsViewDB::GetBestBlock() const {
    uint256 hashBestChain;
    if (!db.Read(DB_BEST_BLOCK, hashBestChain))
        return uint256();
    return hashBestChain;
}

uint256 CCoinsViewDB::GetBestAnchor(ShieldedType type) const {
    uint256 hashBestAnchor;
    
    switch (type) {
        case SPROUT:
            if (!db.Read(DB_BEST_SPROUT_ANCHOR, hashBestAnchor))
                return SproutMerkleTree::empty_root();
            break;
        case SAPLING:
            if (!db.Read(DB_BEST_SAPLING_ANCHOR, hashBestAnchor))
                return SaplingMerkleTree::empty_root();
            break;
        default:
            throw runtime_error("Unknown shielded type");
    }

    return hashBestAnchor;
}

void BatchWriteNullifiers(CDBBatch& batch, CNullifiersMap& mapToUse, const char& dbChar)
{
    for (CNullifiersMap::iterator it = mapToUse.begin(); it != mapToUse.end();) {
        if (it->second.flags & CNullifiersCacheEntry::DIRTY) {
            if (!it->second.entered)
                batch.Erase(make_pair(dbChar, it->first));
            else
                batch.Write(make_pair(dbChar, it->first), true);
            // TODO: changed++? ... See comment in CCoinsViewDB::BatchWrite. If this is needed we could return an int
        }
        CNullifiersMap::iterator itOld = it++;
        mapToUse.erase(itOld);
    }
}

template<typename Map, typename MapIterator, typename MapEntry, typename Tree>
void BatchWriteAnchors(CDBBatch& batch, Map& mapToUse, const char& dbChar)
{
    for (MapIterator it = mapToUse.begin(); it != mapToUse.end();) {
        if (it->second.flags & MapEntry::DIRTY) {
            if (!it->second.entered)
                batch.Erase(make_pair(dbChar, it->first));
            else {
                if (it->first != Tree::empty_root()) {
                    batch.Write(make_pair(dbChar, it->first), it->second.tree);
                }
            }
            // TODO: changed++?
        }
        MapIterator itOld = it++;
        mapToUse.erase(itOld);
    }
}

bool CCoinsViewDB::BatchWrite(CCoinsMap &mapCoins,
                              const uint256 &hashBlock,
                              const uint256 &hashSproutAnchor,
                              const uint256 &hashSaplingAnchor,
                              CAnchorsSproutMap &mapSproutAnchors,
                              CAnchorsSaplingMap &mapSaplingAnchors,
                              CNullifiersMap &mapSproutNullifiers,
                              CNullifiersMap &mapSaplingNullifiers) {
    CDBBatch batch(db);
    size_t count = 0;
    size_t changed = 0;
    for (CCoinsMap::iterator it = mapCoins.begin(); it != mapCoins.end();) {
        if (it->second.flags & CCoinsCacheEntry::DIRTY) {
            if (it->second.coins.IsPruned())
                batch.Erase(make_pair(DB_COINS, it->first));
            else
                batch.Write(make_pair(DB_COINS, it->first), it->second.coins);
            changed++;
        }
        count++;
        CCoinsMap::iterator itOld = it++;
        mapCoins.erase(itOld);
    }

    ::BatchWriteAnchors<CAnchorsSproutMap, CAnchorsSproutMap::iterator, CAnchorsSproutCacheEntry, SproutMerkleTree>(batch, mapSproutAnchors, DB_SPROUT_ANCHOR);
    ::BatchWriteAnchors<CAnchorsSaplingMap, CAnchorsSaplingMap::iterator, CAnchorsSaplingCacheEntry, SaplingMerkleTree>(batch, mapSaplingAnchors, DB_SAPLING_ANCHOR);

    ::BatchWriteNullifiers(batch, mapSproutNullifiers, DB_NULLIFIER);
    ::BatchWriteNullifiers(batch, mapSaplingNullifiers, DB_SAPLING_NULLIFIER);

    if (!hashBlock.IsNull())
        batch.Write(DB_BEST_BLOCK, hashBlock);
    if (!hashSproutAnchor.IsNull())
        batch.Write(DB_BEST_SPROUT_ANCHOR, hashSproutAnchor);
    if (!hashSaplingAnchor.IsNull())
        batch.Write(DB_BEST_SAPLING_ANCHOR, hashSaplingAnchor);

    LogPrint("coindb", "Committing %u changed transactions (out of %u) to coin database...\n", (unsigned int)changed, (unsigned int)count);
    return db.WriteBatch(batch);
}

CBlockTreeDB::CBlockTreeDB(size_t nCacheSize, bool fMemory, bool fWipe, bool compression, int maxOpenFiles) : CDBWrapper(GetDataDir() / "blocks" / "index", nCacheSize, fMemory, fWipe, compression, maxOpenFiles) {
}

bool CBlockTreeDB::ReadBlockFileInfo(int nFile, CBlockFileInfo &info) {
    return Read(make_pair(DB_BLOCK_FILES, nFile), info);
}

bool CBlockTreeDB::WriteReindexing(bool fReindexing) {
    if (fReindexing)
        return Write(DB_REINDEX_FLAG, '1');
    else
        return Erase(DB_REINDEX_FLAG);
}

bool CBlockTreeDB::ReadReindexing(bool &fReindexing) {
    fReindexing = Exists(DB_REINDEX_FLAG);
    return true;
}

bool CBlockTreeDB::ReadLastBlockFile(int &nFile) {
    return Read(DB_LAST_BLOCK, nFile);
}

bool CCoinsViewDB::GetStats(CCoinsStats &stats) const {
    /* It seems that there are no "const iterators" for LevelDB.  Since we
       only need read operations on it, use a const-cast to get around
       that restriction.  */
    boost::scoped_ptr<CDBIterator> pcursor(const_cast<CDBWrapper*>(&db)->NewIterator());
    pcursor->Seek(DB_COINS);

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    stats.hashBlock = GetBestBlock();
    ss << stats.hashBlock;
    CAmount nTotalAmount = 0;
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        CCoins coins;
        if (pcursor->GetKey(key) && key.first == DB_COINS) {
            if (pcursor->GetValue(coins)) {
                stats.nTransactions++;
                for (unsigned int i=0; i<coins.vout.size(); i++) {
                    const CTxOut &out = coins.vout[i];
                    if (!out.IsNull()) {
                        stats.nTransactionOutputs++;
                        ss << VARINT(i+1);
                        ss << out;
                        nTotalAmount += out.nValue;
                    }
                }
                stats.nSerializedSize += 32 + pcursor->GetValueSize();
                ss << VARINT(0);
            } else {
                return error("CCoinsViewDB::GetStats() : unable to read value");
            }
        } else {
            break;
        }
        pcursor->Next();
    }
    {
        LOCK(cs_main);
        stats.nHeight = mapBlockIndex.find(stats.hashBlock)->second->nHeight;
    }
    stats.hashSerialized = ss.GetHash();
    stats.nTotalAmount = nTotalAmount;
    return true;
}

bool CBlockTreeDB::WriteBatchSync(const std::vector<std::pair<int, const CBlockFileInfo*> >& fileInfo, int nLastFile, const std::vector<const CBlockIndex*>& blockinfo) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<int, const CBlockFileInfo*> >::const_iterator it=fileInfo.begin(); it != fileInfo.end(); it++) {
        batch.Write(make_pair(DB_BLOCK_FILES, it->first), *it->second);
    }
    batch.Write(DB_LAST_BLOCK, nLastFile);
    for (std::vector<const CBlockIndex*>::const_iterator it=blockinfo.begin(); it != blockinfo.end(); it++) {
        batch.Write(make_pair(DB_BLOCK_INDEX, (*it)->GetBlockHash()), CDiskBlockIndex(*it));
    }
    return WriteBatch(batch, true);
}

bool CBlockTreeDB::EraseBatchSync(const std::vector<const CBlockIndex*>& blockinfo) {
    CDBBatch batch(*this);
    for (std::vector<const CBlockIndex*>::const_iterator it=blockinfo.begin(); it != blockinfo.end(); it++) {
        batch.Erase(make_pair(DB_BLOCK_INDEX, (*it)->GetBlockHash()));
    }
    return WriteBatch(batch, true);
}

bool CBlockTreeDB::ReadTxIndex(const uint256 &txid, CDiskTxPos &pos) {
    return Read(make_pair(DB_TXINDEX, txid), pos);
}

bool CBlockTreeDB::WriteTxIndex(const std::vector<std::pair<uint256, CDiskTxPos> >&vect) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<uint256,CDiskTxPos> >::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Write(make_pair(DB_TXINDEX, it->first), it->second);
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadSpentIndex(CSpentIndexKey &key, CSpentIndexValue &value) {
    return Read(make_pair(DB_SPENTINDEX, key), value);
}

bool CBlockTreeDB::UpdateSpentIndex(const std::vector<std::pair<CSpentIndexKey, CSpentIndexValue> >&vect) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<CSpentIndexKey,CSpentIndexValue> >::const_iterator it=vect.begin(); it!=vect.end(); it++) {
        if (it->second.IsNull()) {
            batch.Erase(make_pair(DB_SPENTINDEX, it->first));
        } else {
            batch.Write(make_pair(DB_SPENTINDEX, it->first), it->second);
        }
    }
    return WriteBatch(batch);
}

bool CBlockTreeDB::UpdateAddressUnspentIndex(const std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue > >&vect) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator it=vect.begin(); it!=vect.end(); it++) {
        if (it->second.IsNull()) {
            batch.Erase(make_pair(DB_ADDRESSUNSPENTINDEX, it->first));
        } else {
            batch.Write(make_pair(DB_ADDRESSUNSPENTINDEX, it->first), it->second);
        }
    }
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadAddressUnspentIndex(uint160 addressHash, int type,
                                           std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > &unspentOutputs) {

    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    CDataStream ssKeySet(SER_DISK, CLIENT_VERSION);
    ssKeySet << make_pair(DB_ADDRESSUNSPENTINDEX, CAddressIndexIteratorKey(type, addressHash));
    pcursor->Seek(ssKeySet.str());

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        try {
            std::vector<unsigned char> slKey = std::vector<unsigned char>();
            pcursor->GetKey(slKey);
            CDataStream ssKey(slKey, SER_DISK, CLIENT_VERSION);
            char chType;
            CAddressUnspentKey indexKey;
            ssKey >> chType;
            ssKey >> indexKey;
            if (chType == DB_ADDRESSUNSPENTINDEX && indexKey.hashBytes == addressHash) {
                try {
                    std::vector<unsigned char> slValue = std::vector<unsigned char>();
                    pcursor->GetValue(slValue);
                    CDataStream ssValue(slValue, SER_DISK, CLIENT_VERSION);
                    CAddressUnspentValue nValue;
                    ssValue >> nValue;
                    unspentOutputs.push_back(make_pair(indexKey, nValue));
                    pcursor->Next();
                } catch (const std::exception& e) {
                    return error("failed to get address unspent value");
                }
            } else {
                break;
            }
        } catch (const std::exception& e) {
            break;
        }
    }

    return true;
}

bool CBlockTreeDB::WriteAddressIndex(const std::vector<std::pair<CAddressIndexKey, CAmount > >&vect) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<CAddressIndexKey, CAmount> >::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Write(make_pair(DB_ADDRESSINDEX, it->first), it->second);
    return WriteBatch(batch);
}

bool CBlockTreeDB::EraseAddressIndex(const std::vector<std::pair<CAddressIndexKey, CAmount > >&vect) {
    CDBBatch batch(*this);
    for (std::vector<std::pair<CAddressIndexKey, CAmount> >::const_iterator it=vect.begin(); it!=vect.end(); it++)
        batch.Erase(make_pair(DB_ADDRESSINDEX, it->first));
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadAddressIndex(uint160 addressHash, int type,
                                    std::vector<std::pair<CAddressIndexKey, CAmount> > &addressIndex,
                                    int start, int end) {

    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    CDataStream ssKeySet(SER_DISK, CLIENT_VERSION);
    if (start > 0 && end > 0) {
        ssKeySet << make_pair(DB_ADDRESSINDEX, CAddressIndexIteratorHeightKey(type, addressHash, start));
    } else {
        ssKeySet << make_pair(DB_ADDRESSINDEX, CAddressIndexIteratorKey(type, addressHash));
    }
    pcursor->Seek(ssKeySet.str());

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        try {
            std::vector<unsigned char> slKey = std::vector<unsigned char>();
            pcursor->GetKey(slKey);
            CDataStream ssKey(slKey, SER_DISK, CLIENT_VERSION);
            char chType;
            CAddressIndexKey indexKey;
            ssKey >> chType;
            ssKey >> indexKey;
            if (chType == DB_ADDRESSINDEX && indexKey.hashBytes == addressHash) {
                if (end > 0 && indexKey.blockHeight > end) {
                    break;
                }
                try {
                    std::vector<unsigned char> slValue = std::vector<unsigned char>();
                    pcursor->GetValue(slValue);
                    CDataStream ssValue(slValue, SER_DISK, CLIENT_VERSION);
                    CAmount nValue;
                    ssValue >> nValue;

                    addressIndex.push_back(make_pair(indexKey, nValue));
                    pcursor->Next();
                } catch (const std::exception& e) {
                    return error("failed to get address index value");
                }
            } else {
                break;
            }
        } catch (const std::exception& e) {
            break;
        }
    }

    return true;
}

bool getAddressFromIndex(const int &type, const uint160 &hash, std::string &address);

UniValue CBlockTreeDB::Snapshot(int top)
{
    char chType; int64_t total = 0; int64_t totalAddresses = 0; std::string address;
    int64_t utxos = 0; int64_t ignoredAddresses;
    boost::scoped_ptr<CDBIterator> iter(NewIterator());
    std::map <std::string, CAmount> addressAmounts;
    std::vector <std::pair<CAmount, std::string>> vaddr;
    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("start_time", (int) time(NULL)));

    std::map <std::string,int> ignoredMap = {
	{"RReUxSs5hGE39ELU23DfydX8riUuzdrHAE", 1},
	{"RMUF3UDmzWFLSKV82iFbMaqzJpUnrWjcT4", 1},
	{"RA5imhVyJa7yHhggmBytWuDr923j2P1bxx", 1},
	{"RBM5LofZFodMeewUzoMWcxedm3L3hYRaWg", 1},
	{"RAdcko2d94TQUcJhtFHZZjMyWBKEVfgn4J", 1},
	{"RLzUaZ934k2EFCsAiVjrJqM8uU1vmMRFzk", 1},
	{"RMSZMWZXv4FhUgWhEo4R3AQXmRDJ6rsGyt", 1},
	{"RUDrX1v5toCsJMUgtvBmScKjwCB5NaR8py", 1},
	{"RMSZMWZXv4FhUgWhEo4R3AQXmRDJ6rsGyt", 1},
	{"RRvwmbkxR5YRzPGL5kMFHMe1AH33MeD8rN", 1},
	{"RQLQvSgpPAJNPgnpc8MrYsbBhep95nCS8L", 1},
	{"RK8JtBV78HdvEPvtV5ckeMPSTojZPzHUTe", 1},
	{"RHVs2KaCTGUMNv3cyWiG1jkEvZjigbCnD2", 1},
	{"RE3SVaDgdjkRPYA6TRobbthsfCmxQedVgF", 1},
	{"RW6S5Lw5ZCCvDyq4QV9vVy7jDHfnynr5mn", 1},
	{"RTkJwAYtdXXhVsS3JXBAJPnKaBfMDEswF8", 1},
	{"RD6GgnrMpPaTSMn8vai6yiGA7mN4QGPVMY", 1} //Burnaddress for null privkey
    };

    int64_t startingHeight = chainActive.Height();
    //fprintf(stderr, "Starting snapshot at height %lli\n", startingHeight);
    for (iter->SeekToLast(); iter->Valid(); iter->Prev())
    {
        boost::this_thread::interruption_point();
        try
        {
            std::vector<unsigned char> slKey = std::vector<unsigned char>();
            iter->GetKey(slKey);
            CDataStream ssKey(slKey, SER_DISK, CLIENT_VERSION);
            CAddressIndexIteratorKey indexKey;

            ssKey >> chType;
            ssKey >> indexKey;

            //fprintf(stderr, "chType=%d\n", chType);
            if (chType == DB_ADDRESSUNSPENTINDEX)
            {
                try {
                    std::vector<unsigned char> slValue = std::vector<unsigned char>();
                    iter->GetValue(slValue);
                    CDataStream ssValue(slValue, SER_DISK, CLIENT_VERSION);
                    CAmount nValue;
                    ssValue >> nValue;

                    getAddressFromIndex(indexKey.type, indexKey.hashBytes, address);

                    std::map <std::string, int>::iterator ignored = ignoredMap.find(address);
                    if (ignored != ignoredMap.end()) {
                    fprintf(stderr,"ignoring %s\n", address.c_str());
                    ignoredAddresses++;
                    continue;
                    }

                    std::map <std::string, CAmount>::iterator pos = addressAmounts.find(address);
                    if (pos == addressAmounts.end()) {
                    // insert new address + utxo amount
                    //fprintf(stderr, "inserting new address %s with amount %li\n", address.c_str(), nValue);
                    addressAmounts[address] = nValue;
                    totalAddresses++;
                    } else {
                    // update unspent tally for this address
                    //fprintf(stderr, "updating address %s with new utxo amount %li\n", address.c_str(), nValue);
                    addressAmounts[address] += nValue;
                    }
                    //fprintf(stderr,"{\"%s\", %.8f},\n",address.c_str(),(double)nValue/COIN);
                    // total += nValue;
                    utxos++;
                } catch (const std::exception& e) {
                    fprintf(stderr, "DONE %s: LevelDB addressindex exception! - %s\n", __func__, e.what());
                    break;
                }
	        }
        } catch (const std::exception& e) {
	        fprintf(stderr, "DONE reading index entries\n");
            break;
        }
    }

    UniValue addresses(UniValue::VARR);
    //fprintf(stderr, "total=%f, totalAddresses=%li, utxos=%li, ignored=%li\n", (double) total / COIN, totalAddresses, utxos, ignoredAddresses);

    for (std::pair<std::string, CAmount> element : addressAmounts) {
	vaddr.push_back( make_pair(element.second, element.first) );
    }
    std::sort(vaddr.rbegin(), vaddr.rend());

    UniValue obj(UniValue::VOBJ);
    UniValue addressesSorted(UniValue::VARR);
    int topN = 0;
    for (std::vector<std::pair<CAmount, std::string>>::iterator it = vaddr.begin(); it!=vaddr.end(); ++it) {
	UniValue obj(UniValue::VOBJ);
	obj.push_back( make_pair("addr", it->second.c_str() ) );
	char amount[32];
	sprintf(amount, "%.8f", (double) it->first / COIN);
	obj.push_back( make_pair("amount", amount) );
	total += it->first;
	addressesSorted.push_back(obj);
	topN++;
	// If requested, only show top N addresses in output JSON
 	if (top == topN)
	    break;
    }

    if (top)
	totalAddresses = top;

    if (totalAddresses > 0) {
	// Array of all addreses with balances
        result.push_back(make_pair("addresses", addressesSorted));
	// Total amount in this snapshot, which is less than circulating supply if top parameter is used
        result.push_back(make_pair("total", (double) total / COIN ));
	// Average amount in each address of this snapshot
        result.push_back(make_pair("average",(double) (total/COIN) / totalAddresses ));
    }
    // Total number of utxos processed in this snaphot
    result.push_back(make_pair("utxos", utxos));
    // Total number of addresses in this snaphot
    result.push_back(make_pair("total_addresses", totalAddresses));
    // Total number of ignored addresses in this snaphot
    result.push_back(make_pair("ignored_addresses", ignoredAddresses));
    // The snapshot began at this block height
    result.push_back(make_pair("start_height", startingHeight));
    // The snapshot finished at this block height
    result.push_back(make_pair("ending_height", chainActive.Height()));
    return(result);
}

bool CBlockTreeDB::WriteTimestampIndex(const CTimestampIndexKey &timestampIndex) {
    CDBBatch batch(*this);
    batch.Write(make_pair(DB_TIMESTAMPINDEX, timestampIndex), 0);
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadTimestampIndex(const unsigned int &high, const unsigned int &low, const bool fActiveOnly, std::vector<std::pair<uint256, unsigned int> > &hashes) {

    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    CDataStream ssKeySet(SER_DISK, CLIENT_VERSION);
    ssKeySet << make_pair(DB_TIMESTAMPINDEX, CTimestampIndexIteratorKey(low));
    pcursor->Seek(ssKeySet.str());

    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        try {
            std::vector<unsigned char> slKey = std::vector<unsigned char>();
            pcursor->GetKey(slKey);
            CDataStream ssKey(slKey, SER_DISK, CLIENT_VERSION);
            char chType;
            CTimestampIndexKey indexKey;
            ssKey >> chType;
            ssKey >> indexKey;
            if (chType == DB_TIMESTAMPINDEX && indexKey.timestamp < high) {
                if (fActiveOnly) {
                    if (blockOnchainActive(indexKey.blockHash)) {
                        hashes.push_back(std::make_pair(indexKey.blockHash, indexKey.timestamp));
                    }
                } else {
                    hashes.push_back(std::make_pair(indexKey.blockHash, indexKey.timestamp));
                }

                pcursor->Next();
            } else {
                break;
            }
        } catch (const std::exception& e) {
            break;
        }
    }

    return true;
}

bool CBlockTreeDB::WriteTimestampBlockIndex(const CTimestampBlockIndexKey &blockhashIndex, const CTimestampBlockIndexValue &logicalts) {
    CDBBatch batch(*this);
    batch.Write(make_pair(DB_BLOCKHASHINDEX, blockhashIndex), logicalts);
    return WriteBatch(batch);
}

bool CBlockTreeDB::ReadTimestampBlockIndex(const uint256 &hash, unsigned int &ltimestamp) {

    CTimestampBlockIndexValue(lts);
    if (!Read(std::make_pair(DB_BLOCKHASHINDEX, hash), lts))
	return false;

    ltimestamp = lts.ltimestamp;
    return true;
}

bool CBlockTreeDB::WriteFlag(const std::string &name, bool fValue) {
    return Write(std::make_pair(DB_FLAG, name), fValue ? '1' : '0');
}

bool CBlockTreeDB::ReadFlag(const std::string &name, bool &fValue) {
    char ch;
    if (!Read(std::make_pair(DB_FLAG, name), ch))
        return false;
    fValue = ch == '1';
    return true;
}

void komodo_index2pubkey33(uint8_t *pubkey33,CBlockIndex *pindex,int32_t height);

bool CBlockTreeDB::blockOnchainActive(const uint256 &hash) {
    CBlockIndex* pblockindex = mapBlockIndex[hash];

    if (!chainActive.Contains(pblockindex)) {
	return false;
    }

    return true;
}

bool CBlockTreeDB::LoadBlockIndexGuts()
{
    boost::scoped_ptr<CDBIterator> pcursor(NewIterator());

    pcursor->Seek(make_pair(DB_BLOCK_INDEX, uint256()));

    // Load mapBlockIndex
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        std::pair<char, uint256> key;
        if (pcursor->GetKey(key) && key.first == DB_BLOCK_INDEX) {
            CDiskBlockIndex diskindex;
            if (pcursor->GetValue(diskindex)) {
                // Construct block index object
                CBlockIndex* pindexNew = InsertBlockIndex(diskindex.GetBlockHash());
                pindexNew->pprev          = InsertBlockIndex(diskindex.hashPrev);
                pindexNew->nHeight        = diskindex.nHeight;
                pindexNew->nFile          = diskindex.nFile;
                pindexNew->nDataPos       = diskindex.nDataPos;
                pindexNew->nUndoPos       = diskindex.nUndoPos;
                pindexNew->hashSproutAnchor     = diskindex.hashSproutAnchor;
                pindexNew->nVersion       = diskindex.nVersion;
                pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
                pindexNew->hashFinalSaplingRoot   = diskindex.hashFinalSaplingRoot;
                pindexNew->nTime          = diskindex.nTime;
                pindexNew->nBits          = diskindex.nBits;
                pindexNew->nNonce         = diskindex.nNonce;
                pindexNew->nSolution      = diskindex.nSolution;
                pindexNew->nStatus        = diskindex.nStatus;
                pindexNew->nCachedBranchId = diskindex.nCachedBranchId;
                pindexNew->nTx            = diskindex.nTx;
                pindexNew->nSproutValue   = diskindex.nSproutValue;
                
                // Consistency checks
                auto header = pindexNew->GetBlockHeader();
                if (header.GetHash() != pindexNew->GetBlockHash())
                    return error("LoadBlockIndex(): block header inconsistency detected: on-disk = %s, in-memory = %s",
                                 diskindex.ToString(),  pindexNew->ToString());
                if ( 0 ) // POW will be checked before any block is connected
                {
                    uint8_t pubkey33[33];
                    komodo_index2pubkey33(pubkey33,pindexNew,pindexNew->nHeight);
                    if (!CheckProofOfWork(header,pubkey33,pindexNew->nHeight,Params().GetConsensus()))
                        return error("LoadBlockIndex(): CheckProofOfWork failed: %s", pindexNew->ToString());
                }
                pcursor->Next();
            } else {
                return error("LoadBlockIndex() : failed to read value");
            }
        } else {
            break;
        }
    }

    return true;
}
