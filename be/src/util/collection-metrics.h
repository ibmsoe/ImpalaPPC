// Copyright 2012 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef IMPALA_UTIL_COLLECTION_METRICS_H
#define IMPALA_UTIL_COLLECTION_METRICS_H

#include "util/metrics.h"

#include <string>
#include <vector>
#include <set>
#include <boost/algorithm/string/join.hpp>
#include <boost/foreach.hpp>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/count.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/variance.hpp>

#include "util/pretty-printer.h"

namespace impala {

/// Collection metrics are those whose values have more structure than simple
/// scalar types. Therefore they need specialised ToJson() methods, and
/// typically a specialised API for updating the values they contain.

/// Metric whose value is a set of items
template <typename T>
class SetMetric : public Metric {
 public:
  SetMetric(const std::string& key, const std::set<T>& value,
      const std::string& description = "") : Metric(key, description), value_(value) { }

  /// Put an item in this set.
  void Add(const T& item) {
    boost::lock_guard<boost::mutex> l(lock_);
    value_.insert(item);
  }

  /// Remove an item from this set by value.
  void Remove(const T& item) {
    boost::lock_guard<boost::mutex> l(lock_);
    value_.erase(item);
  }

  void Reset() { value_.clear(); }

  virtual void ToJson(rapidjson::Document* document, rapidjson::Value* value) {
    rapidjson::Value container(rapidjson::kObjectType);
    AddStandardFields(document, &container);
    rapidjson::Value metric_list(rapidjson::kArrayType);
    BOOST_FOREACH(const T& s, value_) {
      rapidjson::Value entry_value;
      ToJsonValue(s, TUnit::NONE, document, &entry_value);
      metric_list.PushBack(entry_value, document->GetAllocator());
    }
    container.AddMember("items", metric_list, document->GetAllocator());
    *value = container;
  }

  virtual void ToLegacyJson(rapidjson::Document* document) {
    rapidjson::Value metric_list(rapidjson::kArrayType);
    BOOST_FOREACH(const T& s, value_) {
      rapidjson::Value entry_value;
      ToJsonValue(s, TUnit::NONE, document, &entry_value);
      metric_list.PushBack(entry_value, document->GetAllocator());
    }
    document->AddMember(key_.c_str(), metric_list, document->GetAllocator());
  }

  virtual std::string ToHumanReadable() {
    std::stringstream out;
    PrettyPrinter::PrintStringList<std::set<T> >(
        value_, TUnit::NONE, &out);
    return out.str();
  }

 private:
  /// Lock protecting the set
  boost::mutex lock_;

  /// The set of items
  std::set<T> value_;
};

/// Metric which accumulates min, max and mean of all values, plus a count of samples seen.
/// Printed output looks like:
/// name: count: 4, last: 0.0141, min: 4.546e-06, max: 0.0243, mean: 0.0336, stddev: 0.0336
//
/// After construction, all statistics are ill-defined, but count will be 0. The first call
/// to Update() will initialise all stats.
template <typename T>
class StatsMetric : public Metric {
 public:
  StatsMetric(const std::string& key, const TUnit::type unit,
      const std::string& description = "") : Metric(key, description), unit_(unit) { }

  void Update(const T& value) {
    boost::lock_guard<boost::mutex> l(lock_);
    value_ = value;
    acc_(value);
  }

  void Reset() {
    boost::lock_guard<boost::mutex> l(lock_);
    acc_ = Accumulator();
  }

  virtual void ToJson(rapidjson::Document* document, rapidjson::Value* val) {
    boost::lock_guard<boost::mutex> l(lock_);
    rapidjson::Value container(rapidjson::kObjectType);
    AddStandardFields(document, &container);
    rapidjson::Value units(PrintTUnit(unit_).c_str(), document->GetAllocator());
    container.AddMember("units", units, document->GetAllocator());

    container.AddMember("count", boost::accumulators::count(acc_),
        document->GetAllocator());

    if (boost::accumulators::count(acc_) > 0) {
      container.AddMember("last", value_, document->GetAllocator());
      container.AddMember("min", boost::accumulators::min(acc_),
          document->GetAllocator());
      container.AddMember("max", boost::accumulators::max(acc_),
          document->GetAllocator());
      container.AddMember("mean", boost::accumulators::mean(acc_),
          document->GetAllocator());
      container.AddMember("stddev", sqrt(boost::accumulators::variance(acc_)),
          document->GetAllocator());
    }
    *val = container;
  }

  virtual void ToLegacyJson(rapidjson::Document* document) {
    std::stringstream ss;
    boost::lock_guard<boost::mutex> l(lock_);
    rapidjson::Value container(rapidjson::kObjectType);
    container.AddMember("count", boost::accumulators::count(acc_),
        document->GetAllocator());

    if (boost::accumulators::count(acc_) > 0) {
      container.AddMember("last", value_, document->GetAllocator());
      container.AddMember("min", boost::accumulators::min(acc_),
          document->GetAllocator());
      container.AddMember("max", boost::accumulators::max(acc_),
          document->GetAllocator());
      container.AddMember("mean", boost::accumulators::mean(acc_),
          document->GetAllocator());
      container.AddMember("stddev", sqrt(boost::accumulators::variance(acc_)),
          document->GetAllocator());
    }
    document->AddMember(key_.c_str(), container, document->GetAllocator());
  }

  virtual std::string ToHumanReadable() {
    std::stringstream out;
    out << "count: " << boost::accumulators::count(acc_);
    if (boost::accumulators::count(acc_) > 0) {
      out << ", last: " << PrettyPrinter::Print(value_, unit_)
          << ", min: " << PrettyPrinter::Print(boost::accumulators::min(acc_), unit_)
          << ", max: " << PrettyPrinter::Print(boost::accumulators::max(acc_), unit_)
          << ", mean: " << PrettyPrinter::Print(boost::accumulators::mean(acc_), unit_)
          << ", stddev: " << PrettyPrinter::Print(
              sqrt(boost::accumulators::variance(acc_)), unit_);
    }
    return out.str();
  }

 private:
  /// The units of the values captured in this metric, used when pretty-printing.
  TUnit::type unit_;

  /// Lock protecting the value and the accumulator_set
  boost::mutex lock_;

  /// The last value
  T value_;

  /// The set of accumulators that update the statistics on each Update()
  typedef boost::accumulators::accumulator_set<T,
      boost::accumulators::features<boost::accumulators::tag::mean,
                                    boost::accumulators::tag::count,
                                    boost::accumulators::tag::min,
                                    boost::accumulators::tag::max,
                                    boost::accumulators::tag::variance> > Accumulator;
  Accumulator acc_;

};

};

#endif
