/*
 * Copyright (C) 2015 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(INDEXED_DATABASE)

#include "IDBDatabaseInfo.h"
#include "IDBError.h"
#include <wtf/MainThread.h>

namespace WebCore {

class IDBCursorInfo;
class IDBGetAllResult;
class IDBGetResult;
class IDBIndexInfo;
class IDBKeyData;
class IDBObjectStoreInfo;
class IDBResourceIdentifier;
class IDBTransactionInfo;
class IDBValue;
class ThreadSafeDataBuffer;

enum class IDBGetRecordDataType;

struct IDBGetAllRecordsData;
struct IDBIterateCursorData;
struct IDBKeyRangeData;

namespace IndexedDB {
enum class IndexRecordType;
}

namespace IDBServer {

class IDBBackingStore {
public:
    virtual ~IDBBackingStore() { RELEASE_ASSERT(!isMainThread()); }

    // All functions should take a LockHolder to make sure they have acquired lock when being called.
    // This is used to make sure database operations would not be performed when process is suspended.
    virtual IDBError getOrEstablishDatabaseInfo(IDBDatabaseInfo&, const LockHolder&) = 0;

    virtual IDBError beginTransaction(const IDBTransactionInfo&, const LockHolder&) = 0;
    virtual IDBError abortTransaction(const IDBResourceIdentifier& transactionIdentifier, const LockHolder&) = 0;
    virtual IDBError commitTransaction(const IDBResourceIdentifier& transactionIdentifier, const LockHolder&) = 0;

    virtual IDBError createObjectStore(const IDBResourceIdentifier& transactionIdentifier, const IDBObjectStoreInfo&, const LockHolder&) = 0;
    virtual IDBError deleteObjectStore(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, const LockHolder&) = 0;
    virtual IDBError renameObjectStore(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, const String& newName, const LockHolder&) = 0;
    virtual IDBError clearObjectStore(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, const LockHolder&) = 0;
    virtual IDBError createIndex(const IDBResourceIdentifier& transactionIdentifier, const IDBIndexInfo&, const LockHolder&) = 0;
    virtual IDBError deleteIndex(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, uint64_t indexIdentifier, const LockHolder&) = 0;
    virtual IDBError renameIndex(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, uint64_t indexIdentifier, const String& newName, const LockHolder&) = 0;
    virtual IDBError keyExistsInObjectStore(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, const IDBKeyData&, bool& keyExists, const LockHolder&) = 0;
    virtual IDBError deleteRange(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, const IDBKeyRangeData&, const LockHolder&) = 0;
    virtual IDBError addRecord(const IDBResourceIdentifier& transactionIdentifier, const IDBObjectStoreInfo&, const IDBKeyData&, const IDBValue&, const LockHolder&) = 0;
    virtual IDBError getRecord(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, const IDBKeyRangeData&, IDBGetRecordDataType, IDBGetResult& outValue, const LockHolder&) = 0;
    virtual IDBError getAllRecords(const IDBResourceIdentifier& transactionIdentifier, const IDBGetAllRecordsData&, IDBGetAllResult& outValue, const LockHolder&) = 0;
    virtual IDBError getIndexRecord(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, uint64_t indexIdentifier, IndexedDB::IndexRecordType, const IDBKeyRangeData&, IDBGetResult& outValue, const LockHolder&) = 0;
    virtual IDBError getCount(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, uint64_t indexIdentifier, const IDBKeyRangeData&, uint64_t& outCount, const LockHolder&) = 0;
    virtual IDBError generateKeyNumber(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, uint64_t& keyNumber, const LockHolder&) = 0;
    virtual IDBError revertGeneratedKeyNumber(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, uint64_t keyNumber, const LockHolder&) = 0;
    virtual IDBError maybeUpdateKeyGeneratorNumber(const IDBResourceIdentifier& transactionIdentifier, uint64_t objectStoreIdentifier, double newKeyNumber, const LockHolder&) = 0;
    virtual IDBError openCursor(const IDBResourceIdentifier& transactionIdentifier, const IDBCursorInfo&, IDBGetResult& outResult, const LockHolder&) = 0;
    virtual IDBError iterateCursor(const IDBResourceIdentifier& transactionIdentifier, const IDBResourceIdentifier& cursorIdentifier, const IDBIterateCursorData&, IDBGetResult& outResult, const LockHolder&) = 0;
    virtual bool prefetchCursor(const IDBResourceIdentifier& transactionIdentifier, const IDBResourceIdentifier& cursorIdentifier, const LockHolder&) = 0;

    virtual IDBObjectStoreInfo* infoForObjectStore(uint64_t objectStoreIdentifier, const LockHolder&) = 0;
    virtual void deleteBackingStore(const LockHolder&) = 0;

    virtual bool supportsSimultaneousTransactions(const LockHolder&) = 0;
    virtual bool isEphemeral(const LockHolder&) = 0;

    virtual void close(const LockHolder&) = 0;

    virtual bool hasTransaction(const IDBResourceIdentifier&, const LockHolder&) const = 0;
protected:
    IDBBackingStore() { RELEASE_ASSERT(!isMainThread()); }
};

} // namespace IDBServer
} // namespace WebCore

#endif // ENABLE(INDEXED_DATABASE)
