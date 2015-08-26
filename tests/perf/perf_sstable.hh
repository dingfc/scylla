/*
 * Copyright 2015 Cloudius Systems
 */

#pragma once
#include "../sstable_test.hh"
#include "sstables/sstables.hh"
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics.hpp>
#include <boost/range/irange.hpp>

using namespace sstables;

class test_env {
public:
    struct conf {
        unsigned partitions;
        unsigned key_size;
        size_t buffer_size;
        sstring dir;
    };

private:
    sstring dir() {
        return _cfg.dir + "/" + to_sstring(engine().cpu_id());
    }

    sstring random_key() {
        sstring key(sstring::initialized_later{}, size_t(_cfg.key_size));
        for (auto& b: key) {
            b = _distribution(_generator);
        }
        return key;
    }

    conf _cfg;
    schema_ptr s;
    std::default_random_engine _generator;
    std::uniform_int_distribution<char> _distribution;
    lw_shared_ptr<memtable> _mt;
    std::vector<lw_shared_ptr<sstable>> _sst;

public:
    test_env(conf cfg) : _cfg(std::move(cfg))
           , s(uncompressed_schema())
           , _distribution('@', '~')
           , _mt(make_lw_shared<memtable>(s))
    {}

    future<> stop() { return make_ready_future<>(); }

    void fill_memtable() {
        for (unsigned i = 0; i < _cfg.partitions; i++) {
            auto key = partition_key::from_deeply_exploded(*s, { boost::any(random_key()) });
            _mt->apply(mutation(key, s));
        }
    }

    using clk = std::chrono::high_resolution_clock;
    static auto now() {
        return clk::now();
    }


    // Mappers below
    future<double> flush_memtable(int idx) {
        auto start = test_env::now();
        size_t partitions = _mt->partition_count();
        return test_setup::create_empty_test_dir(dir()).then([this, idx] {
            auto sst = sstables::test::make_test_sstable(_cfg.buffer_size, "ks", "cf", dir(), idx, sstable::version_types::ka, sstable::format_types::big);
            return sst->write_components(*_mt).then([sst] {});
        }).then([start, partitions] {
            auto end = test_env::now();
            auto duration = std::chrono::duration<double>(end - start).count();
            return partitions / duration;
        });
    }
};

// The function func should carry on with the test, and return the number of partitions processed.
// time_runs will then map reduce it, and return the aggregate partitions / sec for the whole system.
template <typename Func>
future<> time_runs(unsigned iterations, unsigned parallelism, distributed<test_env>& dt, Func func) {
    using namespace boost::accumulators;
    auto acc = make_lw_shared<accumulator_set<double, features<tag::mean, tag::error_of<tag::mean>>>>();
    auto idx = boost::irange(0, int(iterations));
    return do_for_each(idx.begin(), idx.end(), [parallelism, acc, &dt, func] (auto iter) {
        auto idx = boost::irange(0, int(parallelism));
        return parallel_for_each(idx.begin(), idx.end(), [&dt, func, acc] (auto idx) {
            return dt.map_reduce(adder<double>(), func, std::move(idx)).then([acc] (double result) {
                auto& a = *acc;
                a(result);
                return make_ready_future<>();
            });
        });
    }).then([acc, iterations, parallelism] {
        std::cout << sprint("%.2f", mean(*acc)) << " +- " << sprint("%.2f", error_of<tag::mean>(*acc)) << " partitions / sec (" << iterations << " runs, " << parallelism << " concurrent ops)\n";
    });
}
