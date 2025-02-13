#include "cpu_manager.h"
#include "probes.h"

#include "executor_pool_basic.h"
#include "executor_pool_io.h"

namespace NActors {
    LWTRACE_USING(ACTORLIB_PROVIDER);

    TCpuManager::TCpuManager(THolder<TActorSystemSetup>& setup)
        : ExecutorPoolCount(setup->GetExecutorsCount())
        , Config(setup->CpuManager)
    {
        if (setup->Executors) { // Explicit mode w/o united pools
            Executors.Reset(setup->Executors.Release());
        } else {
            Setup();
        }
    }

    void TCpuManager::Setup() {
        TAffinity available;
        available.Current();

        std::vector<i16> poolsWithSharedThreads;
        for (TBasicExecutorPoolConfig& cfg : Config.Basic) {
            if (cfg.HasSharedThread) {
                poolsWithSharedThreads.push_back(cfg.PoolId);
            }
        }
        Shared.reset(new TSharedExecutorPool(Config.Shared, ExecutorPoolCount, poolsWithSharedThreads));
        auto sharedPool = static_cast<TSharedExecutorPool*>(Shared.get());

        ui64 ts = GetCycleCountFast();
        Harmonizer.reset(MakeHarmonizer(ts));
        Harmonizer->SetSharedPool(sharedPool);

        Executors.Reset(new TAutoPtr<IExecutorPool>[ExecutorPoolCount]);

        for (ui32 excIdx = 0; excIdx != ExecutorPoolCount; ++excIdx) {
            Executors[excIdx].Reset(CreateExecutorPool(excIdx));
            if (excIdx < Config.PingInfoByPool.size()) {
                Harmonizer->AddPool(Executors[excIdx].Get(), &Config.PingInfoByPool[excIdx]);
            } else {
                Harmonizer->AddPool(Executors[excIdx].Get());
            }
        }
    }

    void TCpuManager::PrepareStart(TVector<NSchedulerQueue::TReader*>& scheduleReaders, TActorSystem* actorSystem) {
        NSchedulerQueue::TReader* readers;
        ui32 readersCount = 0;
        if (Shared) {
            Shared->Prepare(actorSystem, &readers, &readersCount);
            for (ui32 i = 0; i != readersCount; ++i, ++readers) {
                scheduleReaders.push_back(readers);
            }
        }
        for (ui32 excIdx = 0; excIdx != ExecutorPoolCount; ++excIdx) {
            ui32 readersCount = 0;
            Executors[excIdx]->Prepare(actorSystem, &readers, &readersCount);
            for (ui32 i = 0; i != readersCount; ++i, ++readers) {
                scheduleReaders.push_back(readers);
            }
        }
    }

    void TCpuManager::Start() {
        if (Shared) {
            Shared->Start();
        }
        for (ui32 excIdx = 0; excIdx != ExecutorPoolCount; ++excIdx) {
            Executors[excIdx]->Start();
        }
    }

    void TCpuManager::PrepareStop() {
        for (ui32 excIdx = 0; excIdx != ExecutorPoolCount; ++excIdx) {
            Executors[excIdx]->PrepareStop();
        }
        if (Shared) {
            Shared->PrepareStop();
        }
    }

    void TCpuManager::Shutdown() {
        for (ui32 excIdx = 0; excIdx != ExecutorPoolCount; ++excIdx) {
            Executors[excIdx]->Shutdown();
        }
        for (ui32 round = 0, done = 0; done < ExecutorPoolCount && round < 3; ++round) {
            done = 0;
            for (ui32 excIdx = 0; excIdx != ExecutorPoolCount; ++excIdx) {
                if (Executors[excIdx]->Cleanup()) {
                    ++done;
                }
            }
        }
        if (Shared) {
            Shared->Shutdown();
            Shared->Cleanup();
        }
    }

    void TCpuManager::Cleanup() {
        for (ui32 round = 0, done = 0; done < ExecutorPoolCount; ++round) {
            Y_ABORT_UNLESS(round < 10, "actorsystem cleanup could not be completed in 10 rounds");
            done = 0;
            for (ui32 excIdx = 0; excIdx != ExecutorPoolCount; ++excIdx) {
                if (Executors[excIdx]->Cleanup()) {
                    ++done;
                }
            }
        }
        if (Shared) {
            Shared->Cleanup();
        }
        Executors.Destroy();
        if (Shared) {
            Shared.reset();
        }
    }

    IExecutorPool* TCpuManager::CreateExecutorPool(ui32 poolId) {
        for (TBasicExecutorPoolConfig& cfg : Config.Basic) {
            if (cfg.PoolId == poolId) {
                if (cfg.HasSharedThread) {
                    cfg.Threads -= 1;
                    if (cfg.MaxThreadCount) {
                        cfg.MaxThreadCount -= 1;
                    }
                    auto *sharedPool = static_cast<TSharedExecutorPool*>(Shared.get());
                    auto *pool = new TBasicExecutorPool(cfg, Harmonizer.get());
                    if (pool) {
                        pool->AddSharedThread(sharedPool->GetSharedThread(poolId));
                    }
                    return pool;
                } else {
                    return new TBasicExecutorPool(cfg, Harmonizer.get());
                }
            }
        }
        for (TIOExecutorPoolConfig& cfg : Config.IO) {
            if (cfg.PoolId == poolId) {
                return new TIOExecutorPool(cfg);
            }
        }
        Y_ABORT("missing PoolId: %d", int(poolId));
    }

    TVector<IExecutorPool*> TCpuManager::GetBasicExecutorPools() const {
        TVector<IExecutorPool*> pools;
        for (ui32 idx = 0; idx < ExecutorPoolCount; ++idx) {
            if (auto basicPool = dynamic_cast<TBasicExecutorPool*>(Executors[idx].Get()); basicPool != nullptr) {
                pools.push_back(basicPool);
            }
        }
        return pools;
    }

    void TCpuManager::GetPoolStats(ui32 poolId, TExecutorPoolStats& poolStats, TVector<TExecutorThreadStats>& statsCopy, TVector<TExecutorThreadStats>& sharedStatsCopy) const {
        if (poolId < ExecutorPoolCount) {
            Executors[poolId]->GetCurrentStats(poolStats, statsCopy);
        }
        if (Shared) {
            Shared->GetSharedStats(poolId, sharedStatsCopy);
            auto state = Shared->GetState();
            if (i16 threadIdx = state.BorrowedThreadByPool[poolId]; threadIdx != -1) {
                poolStats.CurrentThreadCount += 0.5;
            }
            if (i16 threadIdx = state.ThreadByPool[poolId]; threadIdx != -1) {
                if (state.PoolByBorrowedThread[threadIdx] == -1) {
                    poolStats.CurrentThreadCount -= 0.5;
                }
            }
        }
    }

}
