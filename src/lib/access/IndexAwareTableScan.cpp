// Copyright (c) 2012 Hasso-Plattner-Institut fuer Softwaresystemtechnik GmbH. All rights reserved.
#include "access/IndexAwareTableScan.h"

#include <memory>

#include "access/system/BasicParser.h"
#include "access/json_converters.h"
#include "access/system/QueryParser.h"
#include "access/expressions/pred_buildExpression.h"

#include "io/StorageManager.h"

#include "storage/InvertedIndex.h"
#include "storage/Store.h"
#include "storage/meta_storage.h"
#include "storage/PointerCalculator.h"
#include "storage/GroupkeyIndex.h"

#include "helper/checked_cast.h"

#include "access/expressions/pred_buildExpression.h"
#include "access/expressions/expression_types.h"
#include "access/IntersectPositions.h"
#include "access/SimpleTableScan.h"


#include <chrono>

namespace hyrise {
namespace access {

namespace {
  auto _ = QueryParser::registerPlanOperation<IndexAwareTableScan>("IndexAwareTableScan");
}

IndexAwareTableScan::IndexAwareTableScan() { }

IndexAwareTableScan::~IndexAwareTableScan() { }

struct GroupkeyIndexFunctor {
  typedef storage::pos_range_t value_type;

  std::string _indexname;
  const Json::Value _indexValue1;
  // const Json::Value &_indexValue2;
  PredicateType::type _predicate_type;

  GroupkeyIndexFunctor( const Json::Value &indexValue1,
                        // AbstractIndexValue *indexValue2,
                        std::string indexname,
                        PredicateType::type predicate_type):
    _indexname(indexname),
    _indexValue1(indexValue1),
    // _indexValue2(indexValue2),
    _predicate_type(predicate_type) {}

  template<typename ValueType>
  value_type operator()() {
    auto index = StorageManager::getInstance()->getInvertedIndex(_indexname);
    auto idx = std::dynamic_pointer_cast<GroupkeyIndex<ValueType>>(index);

    ValueType v1 = json_converter::convert<ValueType>(_indexValue1);

    if (!idx)
      throw std::runtime_error("IndexAwareTable scan only supports GroupKeyIndex: " + _indexname);

    switch (_predicate_type) {
      case PredicateType::EqualsExpressionValue:
        return idx->getPositionsForKey(v1);
        break;
      case PredicateType::GreaterThanExpressionValue:
        return idx->getPositionsForKeyGT(v1);
        break;
      case PredicateType::GreaterThanEqualsExpressionValue:
        return idx->getPositionsForKeyGTE(v1);
        break;
      case PredicateType::LessThanExpressionValue:
        return idx->getPositionsForKeyLT(v1);
        break;
      case PredicateType::LessThanEqualsExpressionValue:
        return idx->getPositionsForKeyLTE(v1);
        break;
      // case PredicateType::BetweenExpression:
      //   return idx->getPositionsForKeyBetween(v1->value, v2->value);
      //   break;
      default:
        throw std::runtime_error("Unsupported predicate type in InvertedIndex");
    }
  }
};


void IndexAwareTableScan::executePlanOperation() {

  // auto start = std::chrono::system_clock::now();
  auto t = input.getTables()[0];
  auto t_store = std::dynamic_pointer_cast<const storage::Store>(t);

  // we only work on stores
  if (!t_store)
    throw std::runtime_error("IndexAwareTableScan only works on stores");

  // execute table scan for the delta
  SimpleTableScan _ts;
  _ts.disablePapiTrace(); // disable papi trace as nested operators breaks it
  _ts.setPredicate(_predicate);
  _ts.setProducesPositions(true);
  _ts.addInput(t_store->getDeltaTable());
  _ts.execute();

  // calculate the index results
  std::vector<storage::pos_range_t> idx_results;
  storage::type_switch<hyrise_basic_types> ts;
  size_t i = 0;
  for (auto functor : _idx_functors) {
    auto idx_result = ts(t->typeOfColumn(_field_definition[i++]), functor);
    idx_results.push_back(idx_result);
  }
  
  // sort so we start with the smallest range
  if (idx_results.size() > 2) {
    std::sort(
      begin(idx_results), end(idx_results),
      [] (storage::pos_range_t a, storage::pos_range_t b) {
        return (a.second-a.first)<(b.second-b.first);
      });
  }
  
  pos_list_t *base = new pos_list_t;
  pos_list_t *tmp_result = new pos_list_t;

  // if we only have one idx result, this is the final one
  if (idx_results.size() == 1) {
    auto r = idx_results[0];
    std::copy(r.first, r.second, std::back_inserter(*base));
  } 
  // otherwise we intersect the first two results
  else if (idx_results.size() >= 2) {
    auto a = idx_results[0];
    auto b = idx_results[1];
    PointerCalculator::intersect_pos_list(a.first, a.second, b.first, b.second, base);
  }

  // if we have more than two results, intersect them too
  if (idx_results.size() > 2) {
    std::vector<storage::pos_range_t>::iterator it = idx_results.begin();
    std::vector<storage::pos_range_t>::iterator it_end = idx_results.end();
    ++it; ++it;
    // storage::pos_range_t base = *(it++);
    for (;it != it_end; ++it) {
      auto r = *it;
      PointerCalculator::intersect_pos_list(r.first, r.second, base->begin(), base->end(), tmp_result);
      *base = *tmp_result;
      tmp_result->clear();
    }
  }
  
  // correct offset for delta and add delta result to idx result
  auto pc_delta = checked_pointer_cast<const PointerCalculator>(_ts.getResultTable());
  const pos_list_t *delta_pos_unadjusted = pc_delta->getPositions();
  pos_t offset = t_store->getMainTable()->size();
  size_t delta_result_size = delta_pos_unadjusted->size();
  size_t idx_result_size = base->size();
  base->resize(idx_result_size + delta_result_size);
  for (size_t i=0; i<delta_result_size; ++i)
    (*base)[i+idx_result_size] = (*delta_pos_unadjusted)[i] + offset;

  addResult(PointerCalculator::create(t, base));  

  delete tmp_result;
}


std::shared_ptr<PlanOperation> IndexAwareTableScan::parse(const Json::Value &data) {

  std::shared_ptr<IndexAwareTableScan> idx_scan = BasicParser<IndexAwareTableScan>::parse(data);

  if (!data.isMember("predicates")) {
    throw std::runtime_error("There is no reason for a IndexAwareScan without predicates");
  }
  if (!data.isMember("tablename")) {
    throw std::runtime_error("IndexAwareScan needs a base table name");
  }

  idx_scan->setPredicate(buildExpression(data["predicates"]));

  PredicateType::type pred_type;
  std::string tablename = data["tablename"].asString();

  for (unsigned i = 0; i < data["predicates"].size(); ++i) {
    const Json::Value &predicate = data["predicates"][i];
    pred_type = parsePredicateType(predicate["type"]);

    if (pred_type == PredicateType::AND) continue;

    if (pred_type != PredicateType::EqualsExpressionValue && 
        pred_type != PredicateType::LessThanExpressionValue && 
        pred_type != PredicateType::GreaterThanExpressionValue &&
        pred_type != PredicateType::LessThanEqualsExpressionValue &&
        pred_type != PredicateType::GreaterThanEqualsExpressionValue)
      throw std::runtime_error("IndexAwareScan: Unsupported predicate type");
    
    if (predicate["f"].isNumeric())
        throw std::runtime_error("For now, IndexAwareScan requires fields to be specified via fieldnames");

    std::string fieldname = predicate["f"].asString();
    std::string indexname = "idx__"+tablename+"__"+fieldname;
    GroupkeyIndexFunctor groupkey_functor(predicate["value"], indexname, pred_type);
    idx_scan->_idx_functors.push_back(groupkey_functor);
    idx_scan->addField(fieldname);
  }

  return idx_scan;
}

const std::string IndexAwareTableScan::vname() {
  return "IndexAwareTableScan";
}

void IndexAwareTableScan::setPredicate(SimpleExpression *c) {
  _predicate = c;
}

}
}

