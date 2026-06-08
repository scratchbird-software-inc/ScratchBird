// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

using System;
using System.Collections.Concurrent;
using System.Threading;

namespace ScratchBird.Data;

internal sealed class ProtocolClientPool
{
    internal readonly record struct PoolStats(
        int ActiveCount,
        int IdleCount,
        int MaxSize,
        int MinSize,
        long BorrowAttempts,
        long Borrowed,
        long Returned,
        long Rejected,
        long Evicted);

    private sealed class PooledClient
    {
        public PooledClient(ProtocolClient client, DateTimeOffset createdUtc)
        {
            Client = client;
            CreatedUtc = createdUtc;
        }

        public ProtocolClient Client { get; }
        public DateTimeOffset CreatedUtc { get; }
    }

    internal sealed class ClientPool
    {
        private readonly ConcurrentQueue<PooledClient> _idle = new();
        private readonly ConcurrentDictionary<ProtocolClient, DateTimeOffset> _active = new();
        private readonly SemaphoreSlim _slots;
        private int _borrowTimeoutMs;
        private long _borrowAttempts;
        private long _borrowed;
        private long _returned;
        private long _rejected;
        private long _evicted;

        public ClientPool(int maxSize, int borrowTimeoutMs)
        {
            MaxSize = Math.Max(1, maxSize);
            _slots = new SemaphoreSlim(MaxSize, MaxSize);
            _borrowTimeoutMs = Math.Max(0, borrowTimeoutMs);
        }

        public int MaxSize { get; set; }
        public int MinSize { get; set; }
        public int BorrowTimeoutMs
        {
            get => Volatile.Read(ref _borrowTimeoutMs);
            set => Volatile.Write(ref _borrowTimeoutMs, Math.Max(0, value));
        }

        public int ActiveCount => _active.Count;
        public int IdleCount => _idle.Count;

        public bool TryBorrow(TimeSpan maxAge, out ProtocolClient protocolClient)
        {
            protocolClient = default!;
            Interlocked.Increment(ref _borrowAttempts);

            if (!TryAcquireSlot())
            {
                return false;
            }

            var now = DateTimeOffset.UtcNow;
            while (_idle.TryDequeue(out var pooled))
            {
                if (!pooled.Client.IsHealthy || IsExpired(pooled.CreatedUtc, now, maxAge))
                {
                    pooled.Client.Close();
                    Interlocked.Increment(ref _evicted);
                    continue;
                }

                if (_active.TryAdd(pooled.Client, pooled.CreatedUtc))
                {
                    protocolClient = pooled.Client;
                    Interlocked.Increment(ref _borrowed);
                    return true;
                }

                pooled.Client.Close();
                Interlocked.Increment(ref _rejected);
            }

            protocolClient = new ProtocolClient();
            _active.TryAdd(protocolClient, now);
            Interlocked.Increment(ref _borrowed);
            return true;
        }

        public void Return(ProtocolClient protocolClient, TimeSpan maxAge)
        {
            if (!_active.TryRemove(protocolClient, out var createdUtc))
            {
                protocolClient.Close();
                return;
            }

            if (protocolClient.IsHealthy && !IsExpired(createdUtc, DateTimeOffset.UtcNow, maxAge))
            {
                _idle.Enqueue(new PooledClient(protocolClient, createdUtc));
                Interlocked.Increment(ref _returned);
            }
            else
            {
                protocolClient.Close();
                Interlocked.Increment(ref _evicted);
            }

            _slots.Release();
        }

        public void Reject(ProtocolClient protocolClient)
        {
            if (_active.TryRemove(protocolClient, out _))
            {
                protocolClient.Close();
                Interlocked.Increment(ref _rejected);
                _slots.Release();
            }
            else
            {
                protocolClient.Close();
            }
        }

        public long BorrowAttempts => Interlocked.Read(ref _borrowAttempts);
        public long Borrowed => Interlocked.Read(ref _borrowed);
        public long Returned => Interlocked.Read(ref _returned);
        public long Rejected => Interlocked.Read(ref _rejected);
        public long Evicted => Interlocked.Read(ref _evicted);

        private bool TryAcquireSlot()
        {
            if (_slots.Wait(0))
            {
                return true;
            }

            // Avoid indefinite block; fallback clients can still be created outside the pool.
            var timeoutMs = BorrowTimeoutMs;
            if (timeoutMs <= 0)
            {
                return false;
            }

            return _slots.Wait(timeoutMs);
        }

        private static bool IsExpired(DateTimeOffset createdUtc, DateTimeOffset now, TimeSpan maxAge)
        {
            return maxAge > TimeSpan.Zero && (now - createdUtc) > maxAge;
        }
    }

    internal sealed class Lease : IDisposable
    {
        private readonly ProtocolClient _client;
        private readonly ScratchBirdConfig _config;
        private readonly ClientPool? _pool;
        private bool _disposed;

        internal Lease(ProtocolClient client, ScratchBirdConfig config, ClientPool? pool = null)
        {
            _client = client;
            _config = config;
            _pool = pool;
        }

        public ProtocolClient Client => _client;

        public void Dispose()
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            Return(_config, _client, _pool);
            GC.SuppressFinalize(this);
        }
    }

    private static readonly ConcurrentDictionary<string, ClientPool> Pools = new();

    internal static ProtocolClient BorrowOrCreate(ScratchBirdConfig config, out Lease lease)
    {
        if (!config.Pooling)
        {
            var client = new ProtocolClient();
            lease = new Lease(client, config);
            return client;
        }

        var key = BuildPoolKey(config);
        var pool = Pools.GetOrAdd(
            key,
            _ => new ClientPool(Math.Max(1, config.MaxPoolSize), Math.Max(0, config.PoolAcquireTimeoutMs))
            {
                MinSize = Math.Max(0, config.MinPoolSize)
            });
        pool.MaxSize = Math.Max(1, config.MaxPoolSize);
        pool.MinSize = Math.Max(0, config.MinPoolSize);
        pool.BorrowTimeoutMs = Math.Max(0, config.PoolAcquireTimeoutMs);

        var maxAge = TimeSpan.FromSeconds(Math.Max(0, config.ConnectionLifetime));
        if (pool.TryBorrow(maxAge, out var protocolClient))
        {
            lease = new Lease(protocolClient, config, pool);
            return protocolClient;
        }

        // Fallback when pool is full: do not track these clients.
        var unpooledClient = new ProtocolClient();
        lease = new Lease(unpooledClient, config);
        return unpooledClient;
    }

    internal static PoolStats? GetStats(ScratchBirdConfig config)
    {
        var key = BuildPoolKey(config);
        if (!Pools.TryGetValue(key, out var pool))
        {
            return null;
        }

        return new PoolStats(
            pool.ActiveCount,
            pool.IdleCount,
            pool.MaxSize,
            pool.MinSize,
            pool.BorrowAttempts,
            pool.Borrowed,
            pool.Returned,
            pool.Rejected,
            pool.Evicted);
    }

    internal static int PoolCount => Pools.Count;

    internal static void Return(ScratchBirdConfig config, ProtocolClient client, ClientPool? pool = null)
    {
        if (!config.Pooling || pool == null)
        {
            client.Close();
            return;
        }

        var maxAge = TimeSpan.FromSeconds(Math.Max(0, config.ConnectionLifetime));
        if (!client.IsHealthy)
        {
            pool.Reject(client);
            return;
        }

        pool.Return(client, maxAge);
    }

    private static string BuildPoolKey(ScratchBirdConfig config)
    {
        return $"{config.FrontDoorMode}|{config.Protocol}|{config.Host}:{config.Port}|{config.Database}|{config.Username}|{config.Schema}|{config.ManagerConnectionProfile}|{config.ManagerClientIntent}|{config.SslMode}|{config.AllowInsecureDisable}|{config.SslRootCert}|{config.SslCert}|{config.ManagerAuthFastPath}|{config.ManagerClientFlags}|{config.MaxPoolSize}|{config.MinPoolSize}|{config.ConnectionLifetime}|{config.PoolAcquireTimeoutMs}";
    }
}
