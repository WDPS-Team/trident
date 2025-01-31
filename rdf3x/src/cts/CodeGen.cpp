#include <cts/codegen/CodeGen.hpp>
#include <cts/infra/QueryGraph.hpp>
#include <cts/plangen/Plan.hpp>

#include <rts/runtime/Runtime.hpp>
//#include <rts/runtime/DifferentialIndex.hpp>

#include <rts/operator/AggregatedIndexScan.hpp>
#include <rts/operator/EmptyScan.hpp>
#include <rts/operator/Filter.hpp>
#include <rts/operator/FullyAggregatedIndexScan.hpp>
#include <rts/operator/HashGroupify.hpp>
#include <rts/operator/HashJoin.hpp>
#include <rts/operator/CartProd.hpp>
#include <rts/operator/IndexScan.hpp>
#include <rts/operator/MergeJoin.hpp>
#include <rts/operator/MergeUnion.hpp>
#include <rts/operator/NestedLoopFilter.hpp>
#include <rts/operator/NestedLoopJoin.hpp>
#include <rts/operator/ResultsPrinter.hpp>
#include <rts/operator/Selection.hpp>
#include <rts/operator/SingletonScan.hpp>
#include <rts/operator/Sort.hpp>
#include <rts/operator/Minus.hpp>
#include <rts/operator/ValuesScan.hpp>
#include <rts/operator/Assignment.hpp>
#include <rts/operator/TableFunction.hpp>
#include <rts/operator/Union.hpp>
#include <rts/operator/DuplLimit.hpp>
#include <rts/operator/GroupBy.hpp>
#include <rts/operator/AggrFunctions.hpp>

#include <trident/sparql/aggrhandler.h>

#include <cstdlib>
#include <map>
#include <set>
#include <cassert>
//---------------------------------------------------------------------------
// RDF-3X
// (c) 2008 Thomas Neumann. Web site: http://www.mpi-inf.mpg.de/~neumann/rdf3x
//
// This work is licensed under the Creative Commons
// Attribution-Noncommercial-Share Alike 3.0 Unported License. To view a copy
// of this license, visit http://creativecommons.org/licenses/by-nc-sa/3.0/
// or send a letter to Creative Commons, 171 Second Street, Suite 300,
// San Francisco, California, 94105, USA.
//---------------------------------------------------------------------------
using namespace std;
//---------------------------------------------------------------------------
static Operator* translatePlan(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan);
//---------------------------------------------------------------------------
static void resolveScanVariable(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, unsigned slot, const QueryGraph::Node& node, Register*& reg, bool& bound, bool unused = false)
    // Resolve a variable used in a scan
{
    bool constant = (slot == 0) ? node.constSubject : ((slot == 1) ? node.constPredicate : node.constObject);
    uint64_t var = (slot == 0) ? node.subject : ((slot == 1) ? node.predicate : node.object);
    reg = runtime.getRegister((*registers.find(&node)).second + slot);
    if (constant) {
        bound = true;
        reg->value = var;
    } else if (unused) {
        bound = false;
        reg = 0;
    } else {
        if (context.count(var)) {
            bound = true;
            reg = (*(context.find(var))).second;
        } else {
            bound = false;
            if (projection.count(var))
                bindings[var] = reg;
        }
    }
}
//---------------------------------------------------------------------------
static Operator* translateValuesScan(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
{
    const QueryGraph::ValuesNode& node = *reinterpret_cast<QueryGraph::ValuesNode*>(plan->left);
    std::vector<Register*> regs;
    for(unsigned i = 0; i < node.variables.size(); ++i) {
        unsigned var = node.variables[i];
        Register *reg = runtime.getRegister(registers.find((QueryGraph::Node*)&node)->second + i);
        regs.push_back(reg);
        bindings[var] = reg;
    }
    return new ValuesScan(regs, node.values, node.values.size() / node.variables.size());
}
//---------------------------------------------------------------------------
static Operator* translateIndexScan(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
    // Translate an index scan into an operator tree
{
    const QueryGraph::Node& node = *reinterpret_cast<QueryGraph::Node*>(plan->right);

    // Initialize the registers
    bool constSubject, constPredicate, constObject;
    Register* subject, *predicate, *object;
    resolveScanVariable(runtime, context, projection, bindings, registers, 0, node, subject, constSubject);
    resolveScanVariable(runtime, context, projection, bindings, registers, 1, node, predicate, constPredicate);
    resolveScanVariable(runtime, context, projection, bindings, registers, 2, node, object, constObject);

    // And return the operator
    //if (runtime.hasDifferentialIndex())
    //    return runtime.getDifferentialIndex().createScan(static_cast<Database::DataOrder>(plan->opArg), subject, constSubject, predicate, constPredicate, object, constObject, plan->cardinality);

    return IndexScan::create(runtime.getDatabase(), static_cast<DBLayer::DataOrder>(plan->opArg),
            subject, constSubject,
            predicate, constPredicate,
            object, constObject,
            plan->cardinality);
}
//---------------------------------------------------------------------------
static Operator* translateAggregatedIndexScan(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
    // Translate an aggregated index scan into an operator tree
{
    const QueryGraph::Node& node = *reinterpret_cast<QueryGraph::Node*>(plan->right);
    DBLayer::DataOrder order = static_cast<DBLayer::DataOrder>(plan->opArg);

    // Initialize the registers
    bool constSubject, constPredicate, constObject;
    Register* subject, *predicate, *object;
    resolveScanVariable(runtime, context, projection, bindings, registers, 0, node, subject, constSubject, (order == DBLayer::Order_Object_Predicate_Subject) || (order == DBLayer::Order_Predicate_Object_Subject));
    resolveScanVariable(runtime, context, projection, bindings, registers, 1, node, predicate, constPredicate, (order == DBLayer::Order_Subject_Object_Predicate) || (order == DBLayer::Order_Object_Subject_Predicate));
    resolveScanVariable(runtime, context, projection, bindings, registers, 2, node, object, constObject, (order == DBLayer::Order_Subject_Predicate_Object) || (order == DBLayer::Order_Predicate_Subject_Object));

    // And return the operator
    //if (runtime.hasDifferentialIndex())
    //return runtime.getDifferentialIndex().createAggregatedScan(static_cast<Database::DataOrder>(plan->opArg), subject, constSubject, predicate, constPredicate, object, constObject, plan->cardinality);
    return AggregatedIndexScan::create(runtime.getDatabase(), order,
            subject, constSubject,
            predicate, constPredicate,
            object, constObject,
            plan->cardinality);
}
//---------------------------------------------------------------------------
static Operator* translateFullyAggregatedIndexScan(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
    // Translate an fully aggregated index scan into an operator tree
{
    const QueryGraph::Node& node = *reinterpret_cast<QueryGraph::Node*>(plan->right);
    DBLayer::DataOrder order = static_cast<DBLayer::DataOrder>(plan->opArg);

    // Initialize the registers
    bool constSubject, constPredicate, constObject;
    Register* subject, *predicate, *object;
    resolveScanVariable(runtime, context, projection, bindings, registers,
            0, node, subject, constSubject,
            (order != DBLayer::Order_Subject_Predicate_Object)
            && (order != DBLayer::Order_Subject_Object_Predicate));
    resolveScanVariable(runtime, context, projection, bindings, registers, 1, node, predicate, constPredicate, (order != DBLayer::Order_Predicate_Subject_Object) && (order != DBLayer::Order_Predicate_Object_Subject));
    resolveScanVariable(runtime, context, projection, bindings, registers, 2, node, object, constObject, (order != DBLayer::Order_Object_Subject_Predicate) && (order != DBLayer::Order_Object_Predicate_Subject));

    // And return the operator
    //if (runtime.hasDifferentialIndex())
    //    return runtime.getDifferentialIndex().createFullyAggregatedScan(static_cast<DBLayer::DataOrder>(plan->opArg), subject, constSubject, predicate, constPredicate, object, constObject, plan->cardinality);
    return FullyAggregatedIndexScan::create(runtime.getDatabase(), order,
            subject, constSubject,
            predicate, constPredicate,
            object, constObject,
            plan->cardinality);
}
//---------------------------------------------------------------------------
static void collectVariables(const map<unsigned, Register*>& context, set<unsigned>& variables, Plan* plan)
    // Collect all variables contained in a plan
{
    switch (plan->op) {
        case Plan::IndexScan:
        case Plan::AggregatedIndexScan:
        case Plan::FullyAggregatedIndexScan: {
                                                 const QueryGraph::Node& node = *reinterpret_cast<QueryGraph::Node*>(plan->right);
                                                 if ((!node.constSubject) && (!context.count(node.subject)))
                                                     variables.insert(node.subject);
                                                 if ((!node.constPredicate) && (!context.count(node.predicate)))
                                                     variables.insert(node.predicate);
                                                 if ((!node.constObject) && (!context.count(node.object)))
                                                     variables.insert(node.object);
                                                 break;
                                             }
        case Plan::NestedLoopJoin:
        case Plan::MergeJoin:
        case Plan::HashJoin:
        case Plan::Union:
        case Plan::MergeUnion:
        case Plan::CartProd:
                                             collectVariables(context, variables, plan->left);
                                             collectVariables(context, variables, plan->right);
                                             break;
        case Plan::HashGroupify:
        case Plan::Filter:
                                             collectVariables(context, variables, plan->left);
                                             break;
        case Plan::TableFunction: {
                                      const QueryGraph::TableFunction& func = *reinterpret_cast<QueryGraph::TableFunction*>(plan->right);
                                      for (vector<unsigned>::const_iterator iter = func.output.begin(), limit = func.output.end(); iter != limit; ++iter)
                                          variables.insert(*iter);
                                      collectVariables(context, variables, plan->left);
                                      break;
                                  }
        case Plan::Singleton:
                                  break;
        case Plan::Minus:
                                  collectVariables(context, variables, plan->left);
                                  collectVariables(context, variables, plan->right);
                                  break;
        case Plan::ValuesScan:
                                  for (auto v : ((QueryGraph::ValuesNode*) plan->left)->variables) {
                                      variables.insert(v);
                                  }
                                  break;
        case Plan::GroupBy:
        case Plan::Aggregates:
        case Plan::Having:
                                  collectVariables(context, variables, plan->left);
                                  break;
        case Plan::Subselect:
                                  //Here I collect only the projected variables
                                  QueryGraph *graph = (QueryGraph*)plan->right;
                                  for (QueryGraph::projection_iterator itr = graph->projectionBegin();
                                          itr != graph->projectionEnd(); ++itr) {
                                      variables.insert(*itr);
                                  }
                                  break;
    }
}
//---------------------------------------------------------------------------
static void findScan(Plan *plan, set<unsigned>& variables, int *bitset) {
    *bitset = 0;
    switch (plan->op) {
        case Plan::IndexScan:
        case Plan::AggregatedIndexScan:
        case Plan::FullyAggregatedIndexScan: {
                                                 const QueryGraph::Node& node = *reinterpret_cast<QueryGraph::Node*>(plan->right);
                                                 if ((!node.constSubject) && (variables.count(node.subject)))
                                                     *bitset |= 1;
                                                 if ((!node.constPredicate) && (variables.count(node.predicate)))
                                                     *bitset |= 2;
                                                 if ((!node.constObject) && (variables.count(node.object)))
                                                     *bitset |= 4;
                                                 break;
                                             }
        case Plan::Subselect:
                                             findScan(plan->left, variables, bitset);
                                             break;
        case Plan::HashJoin:
        case Plan::MergeJoin:
                                             findScan(plan->left, variables, bitset);
                                             break;
        default:
                                             break;
    }
}
//---------------------------------------------------------------------------
static void getJoinVariables(const map<unsigned, Register*>& context, set<unsigned>& variables, Plan* left, Plan* right, int *bitset)
    // Get the join variables
{
    // Collect all variables
    set<unsigned> leftVariables, rightVariables;
    collectVariables(context, leftVariables, left);
    collectVariables(context, rightVariables, right);

    // Find common ones
    if (leftVariables.size() < rightVariables.size()) {
        for (set<unsigned>::const_iterator iter = leftVariables.begin(), limit = leftVariables.end(); iter != limit; ++iter)
            if (rightVariables.count(*iter))
                variables.insert(*iter);
    } else {
        for (set<unsigned>::const_iterator iter = rightVariables.begin(), limit = rightVariables.end(); iter != limit; ++iter)
            if (leftVariables.count(*iter))
                variables.insert(*iter);
    }
    // Here, we figure out, if "right" is a scan, where the join variables are: s, p, or o.
    if (bitset != NULL) {
        findScan(right, variables, bitset);
    }
}

//---------------------------------------------------------------------------
static void mergeBindings(const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<unsigned, Register*>& leftBindings, const map<unsigned, Register*>& rightBindings)
    // Merge bindings after a join
{
    for (map<unsigned, Register*>::const_iterator iter = leftBindings.begin(), limit = leftBindings.end(); iter != limit; ++iter)
        if (projection.count((*iter).first))
            bindings[(*iter).first] = (*iter).second;
    for (map<unsigned, Register*>::const_iterator iter = rightBindings.begin(), limit = rightBindings.end(); iter != limit; ++iter)
        if (projection.count((*iter).first) && (!bindings.count((*iter).first)))
            bindings[(*iter).first] = (*iter).second;
}
//---------------------------------------------------------------------------
static Operator* addAdditionalSelections(Runtime& runtime, Operator* input, const set<unsigned>& joinVariables, map<unsigned, Register*>& leftBindings, map<unsigned, Register*>& rightBindings, unsigned joinedOn)
    // Convert additional join predicates into a selection
{
    // Examine join conditions
    vector<Register*> left, right;
    for (set<unsigned>::const_iterator iter = joinVariables.begin(), limit = joinVariables.end(); iter != limit; ++iter) {
        if ((*iter) != joinedOn) {
            left.push_back(leftBindings[*iter]);
            right.push_back(rightBindings[*iter]);
        }
    }

    // Build the results
    if (!left.empty()) {
        Selection::Predicate* predicate = 0;
        for (unsigned index = 0; index < left.size(); index++) {
            Selection::Predicate* p = new Selection::Equal(new Selection::Variable(left[index]), new Selection::Variable(right[index]));
            if (predicate)
                predicate = new Selection::And(predicate, p);
            else
                predicate = p;
        }
        return new Selection(input, runtime, predicate, input->getExpectedOutputCardinality());
    } else  {
        return input;
    }
}
//---------------------------------------------------------------------------
static Operator* translateNestedLoopJoin(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
    // Translate a nested loop join into an operator tree
{
    // Get the join variables (if any)
    set<unsigned> joinVariables, newProjection = projection;
    getJoinVariables(context, joinVariables, plan->left, plan->right, NULL);
    newProjection.insert(joinVariables.begin(), joinVariables.end());

    // Build the input trees
    map<unsigned, Register*> leftBindings, rightBindings;
    Operator* leftTree = translatePlan(runtime, context, newProjection, leftBindings, registers, plan->left);
    Operator* rightTree = translatePlan(runtime, context, newProjection, rightBindings, registers, plan->right);
    mergeBindings(projection, bindings, leftBindings, rightBindings);

    // Build the operator
    Operator* result = new NestedLoopJoin(leftTree, rightTree, plan->cardinality);

    // And apply additional selections if necessary
    result = addAdditionalSelections(runtime, result, joinVariables, leftBindings, rightBindings, ~0u);

    return result;
}
//---------------------------------------------------------------------------
static Operator* translateMergeJoin(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
    // Translate a merge join into an operator tree
{
    // Get the join variables (if any)
    set<unsigned> joinVariables, newProjection = projection;
    getJoinVariables(context, joinVariables, plan->left, plan->right, NULL);
    newProjection.insert(joinVariables.begin(), joinVariables.end());
    assert(!joinVariables.empty());
    unsigned joinOn = plan->opArg;
    assert(joinVariables.count(joinOn));

    // Build the input trees
    map<unsigned, Register*> leftBindings, rightBindings;
    Operator* leftTree = translatePlan(runtime, context, newProjection, leftBindings, registers, plan->left);
    Operator* rightTree = translatePlan(runtime, context, newProjection, rightBindings, registers, plan->right);
    mergeBindings(projection, bindings, leftBindings, rightBindings);

    // Prepare the tails
    vector<Register*> leftTail, rightTail;
    for (map<unsigned, Register*>::const_iterator iter = leftBindings.begin(), limit = leftBindings.end(); iter != limit; ++iter)
        if ((*iter).first != joinOn)
            leftTail.push_back((*iter).second);
    for (map<unsigned, Register*>::const_iterator iter = rightBindings.begin(), limit = rightBindings.end(); iter != limit; ++iter)
        if ((*iter).first != joinOn)
            rightTail.push_back((*iter).second);

    // Build the operator
    Operator* result = new MergeJoin(leftTree, leftBindings[joinOn], leftTail, rightTree, rightBindings[joinOn], rightTail, plan->left->optional, plan->right->optional, plan->cardinality);

    // And apply additional selections if necessary
    result = addAdditionalSelections(runtime, result, joinVariables, leftBindings, rightBindings, joinOn);

    return result;
}
//---------------------------------------------------------------------------
static Operator* translateHashJoin(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
    // Translate a hash join into an operator tree
{
    // Get the join variables (if any)
    set<unsigned> joinVariables, newProjection = projection;
    int bitset;
    getJoinVariables(context, joinVariables, plan->left, plan->right, &bitset);
    newProjection.insert(joinVariables.begin(), joinVariables.end());
    assert(!joinVariables.empty());
    unsigned joinOn = *(joinVariables.begin());

    // Build the input trees
    map<unsigned, Register*> leftBindings, rightBindings;
    Operator* leftTree = translatePlan(runtime, context, newProjection, leftBindings, registers, plan->left);
    Operator* rightTree = translatePlan(runtime, context, newProjection, rightBindings, registers, plan->right);
    mergeBindings(projection, bindings, leftBindings, rightBindings);

    // Prepare the tails
    vector<Register*> leftTail, rightTail;
    for (map<unsigned, Register*>::const_iterator iter = leftBindings.begin(), limit = leftBindings.end(); iter != limit; ++iter)
        if ((*iter).first != joinOn)
            leftTail.push_back((*iter).second);
    for (map<unsigned, Register*>::const_iterator iter = rightBindings.begin(), limit = rightBindings.end(); iter != limit; ++iter)
        if ((*iter).first != joinOn)
            rightTail.push_back((*iter).second);

    // Build the operator
    Operator* result = new HashJoin(leftTree, leftBindings[joinOn], leftTail, rightTree, rightBindings[joinOn], rightTail, -plan->left->costs, plan->right->costs, plan->cardinality, plan->left->optional, plan->right->optional, bitset);

    // And apply additional selections if necessary
    result = addAdditionalSelections(runtime, result, joinVariables, leftBindings, rightBindings, joinOn);

    return result;
}
//---------------------------------------------------------------------------
static Operator* translateCartProd(Runtime& runtime,
        const map<unsigned, Register*>& context,
        const set<unsigned>& projection,
        map<unsigned, Register*>& bindings,
        const map<const QueryGraph::Node*, unsigned>& registers,
        Plan* plan)
{
    // Build the input trees
    int bitset = 0;
    map<unsigned, Register*> leftBindings, rightBindings;
    Operator* leftTree = translatePlan(runtime, context, projection, leftBindings, registers, plan->left);
    Operator* rightTree = translatePlan(runtime, context, projection, rightBindings, registers, plan->right);
    mergeBindings(projection, bindings, leftBindings, rightBindings);

    // Prepare the tails
    vector<Register*> leftTail, rightTail;
    for (map<unsigned, Register*>::const_iterator iter = leftBindings.begin(), limit = leftBindings.end(); iter != limit; ++iter)
        leftTail.push_back((*iter).second);
    for (map<unsigned, Register*>::const_iterator iter = rightBindings.begin(), limit = rightBindings.end(); iter != limit; ++iter)
        rightTail.push_back((*iter).second);

    // Build the operator
    Operator* result = new CartProd(leftTree,
            leftTail, rightTree,
            rightTail, plan->cardinality, plan->left->optional,
            plan->right->optional, bitset);
    return result;
}
//---------------------------------------------------------------------------
static Operator* translateHashGroupify(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
    // Translate a hash groupify into an operator tree
{
    // Build the input trees
    Operator* tree = translatePlan(runtime, context, projection, bindings, registers, plan->left);

    // Collect output registers
    vector<Register*> output;
    for (map<unsigned, Register*>::const_iterator iter = bindings.begin(), limit = bindings.end(); iter != limit; ++iter)
        output.push_back((*iter).second);

    // Build the operator
    return new HashGroupify(tree, output, plan->cardinality);
}
//---------------------------------------------------------------------------
static void collectVariables(set<unsigned>& filterVariables, const QueryGraph::Filter& filter)
    // Collect all query variables
{
    if (filter.type == QueryGraph::Filter::Variable)
        filterVariables.insert(filter.id);
    if (filter.arg1)
        collectVariables(filterVariables, *filter.arg1);
    if (filter.arg2)
        collectVariables(filterVariables, *filter.arg2);
    if (filter.arg3)
        collectVariables(filterVariables, *filter.arg3);
}
//---------------------------------------------------------------------------
static Selection::Predicate* buildSelection(Runtime &runtime, const map<unsigned, Register*>& bindings, const QueryGraph::Filter& filter, Plan *plan, const map<const QueryGraph::Node*, unsigned>& registers);
//---------------------------------------------------------------------------
static void collectSelectionArgs(Runtime &runtime, const map<unsigned, Register*>& bindings, vector<Selection::Predicate*>& args, const QueryGraph::Filter* input, Plan *filterplan, const map<const QueryGraph::Node*, unsigned>& registers)
    // Collect all function arguments
{
    for (const QueryGraph::Filter* iter = input; iter; iter = iter->arg2) {
        assert(iter->type == QueryGraph::Filter::ArgumentList);
        args.push_back(buildSelection(runtime, bindings, *(iter->arg1), filterplan, registers));
    }
}
//---------------------------------------------------------------------------
static Selection::Predicate* buildSelection(Runtime &runtime, const map<unsigned, Register*>& bindings, const QueryGraph::Filter& filter, Plan *plan, const map<const QueryGraph::Node*, unsigned>& registers)
    // Construct a complex filter predicate
{
    switch (filter.type) {
        case QueryGraph::Filter::Or:
            return new Selection::Or(buildSelection(runtime, bindings, *filter.arg1, plan, registers), buildSelection(runtime, bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::And:
            return new Selection::And(buildSelection(runtime, bindings, *filter.arg1, plan, registers), buildSelection(runtime, bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::Equal:
            return new Selection::Equal(buildSelection(runtime, bindings, *filter.arg1, plan, registers), buildSelection(runtime, bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::NotEqual:
            return new Selection::NotEqual(buildSelection(runtime, bindings, *filter.arg1, plan, registers), buildSelection(runtime,bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::Less:
            return new Selection::Less(buildSelection(runtime,bindings, *filter.arg1, plan, registers), buildSelection(runtime,bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::LessOrEqual:
            return new Selection::LessOrEqual(buildSelection(runtime,bindings, *filter.arg1, plan, registers), buildSelection(runtime,bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::Greater:
            return new Selection::Less(buildSelection(runtime,bindings, *filter.arg2, plan, registers), buildSelection(runtime,bindings, *filter.arg1, plan, registers));
        case QueryGraph::Filter::GreaterOrEqual:
            return new Selection::LessOrEqual(buildSelection(runtime,bindings, *filter.arg2, plan, registers), buildSelection(runtime,bindings, *filter.arg1, plan, registers));
        case QueryGraph::Filter::Plus:
            return new Selection::Plus(buildSelection(runtime,bindings, *filter.arg1, plan, registers), buildSelection(runtime,bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::Minus:
            return new Selection::Minus(buildSelection(runtime,bindings, *filter.arg1, plan, registers), buildSelection(runtime,bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::Mul:
            return new Selection::Mul(buildSelection(runtime,bindings, *filter.arg1, plan, registers), buildSelection(runtime,bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::Div:
            return new Selection::Div(buildSelection(runtime,bindings, *filter.arg1, plan, registers), buildSelection(runtime,bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::Not:
            return new Selection::Not(buildSelection(runtime,bindings, *filter.arg1, plan, registers));
        case QueryGraph::Filter::UnaryPlus:
            return buildSelection(runtime,bindings, *filter.arg1, plan, registers);
        case QueryGraph::Filter::UnaryMinus:
            return new Selection::Neg(buildSelection(runtime,bindings, *filter.arg1, plan, registers));
        case QueryGraph::Filter::Literal:
            if (~filter.id)
                return new Selection::ConstantLiteral(filter.id);
            else
                return new Selection::TemporaryConstantLiteral(filter.value);
        case QueryGraph::Filter::Variable:
            if (bindings.count(filter.id))
                return new Selection::Variable((*bindings.find(filter.id)).second);
            else
                return new Selection::Null();
        case QueryGraph::Filter::IRI:
            if (~filter.id)
                return new Selection::ConstantIRI(filter.id);
            else
                return new Selection::TemporaryConstantIRI(filter.value);
        case QueryGraph::Filter::Null:
            return new Selection::Null();
        case QueryGraph::Filter::Function: {
                                               assert(filter.arg1->type == QueryGraph::Filter::IRI);
                                               vector<Selection::Predicate*> args;
                                               collectSelectionArgs(runtime, bindings, args, filter.arg2, plan, registers);
                                               return new Selection::FunctionCall(filter.arg1->value, args);
                                           }
        case QueryGraph::Filter::ArgumentList:
                                           assert(false); // cannot happen
        case QueryGraph::Filter::Builtin_str:
                                           return new Selection::BuiltinStr(buildSelection(runtime,bindings, *filter.arg1, plan, registers));
        case QueryGraph::Filter::Builtin_lang:
                                           return new Selection::BuiltinLang(buildSelection(runtime,bindings, *filter.arg1, plan, registers));
        case QueryGraph::Filter::Builtin_langmatches:
                                           return new Selection::BuiltinLangMatches(buildSelection(runtime,bindings, *filter.arg1, plan, registers), buildSelection(runtime,bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::Builtin_contains:
                                           return new Selection::BuiltinContains(buildSelection(runtime,bindings, *filter.arg1, plan, registers), buildSelection(runtime,bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::Builtin_datatype:
                                           return new Selection::BuiltinDatatype(buildSelection(runtime,bindings, *filter.arg1, plan, registers));
        case QueryGraph::Filter::Builtin_bound:
                                           if (~(filter.arg1->id))
                                               return new Selection::BuiltinBound((*bindings.find(filter.arg1->id)).second);
                                           else
                                               return new Selection::False();
        case QueryGraph::Filter::Builtin_sameterm:
                                           return new Selection::BuiltinSameTerm(buildSelection(runtime,bindings, *filter.arg1, plan, registers), buildSelection(runtime,bindings, *filter.arg2, plan, registers));
        case QueryGraph::Filter::Builtin_isiri:
                                           return new Selection::BuiltinIsIRI(buildSelection(runtime,bindings, *filter.arg1, plan, registers));
        case QueryGraph::Filter::Builtin_isblank:
                                           return new Selection::BuiltinIsBlank(buildSelection(runtime,bindings, *filter.arg1, plan, registers));
        case QueryGraph::Filter::Builtin_isliteral:
                                           return new Selection::BuiltinIsLiteral(buildSelection(runtime,bindings, *filter.arg1, plan, registers));
        case QueryGraph::Filter::Builtin_regex:
                                           return new Selection::BuiltinRegEx(buildSelection(runtime,bindings, *filter.arg1, plan, registers), buildSelection(runtime,bindings, *filter.arg2, plan, registers), filter.arg3 ? buildSelection(runtime,bindings, *filter.arg3, plan, registers) : 0);
        case QueryGraph::Filter::Builtin_replace:
                                           return new Selection::BuiltinReplace(buildSelection(runtime,bindings, *filter.arg1, plan, registers), buildSelection(runtime,bindings, *filter.arg2, plan, registers), filter.arg3 ? buildSelection(runtime,bindings, *filter.arg3, plan, registers) : 0, filter.arg4 ? buildSelection(runtime,bindings, *filter.arg4, plan, registers) : 0);
        case QueryGraph::Filter::Builtin_in: {
                                                 vector<Selection::Predicate*> args;
                                                 set<string> strings;
                                                 collectSelectionArgs(runtime, bindings, args, filter.arg2, plan, registers);
                                                 return new Selection::BuiltinIn(buildSelection(runtime,bindings, *filter.arg1, plan, registers), args, strings, true);
                                             }
        case QueryGraph::Filter::Builtin_notin: {
                                                    vector<Selection::Predicate*> args;
                                                    set<string> strings;
                                                    for (const QueryGraph::Filter* iter = filter.arg2; iter; iter = iter->arg1) {
                                                        assert(iter->type == QueryGraph::Filter::ArgumentList);
                                                        strings.insert(iter->value);
                                                    }
                                                    return new Selection::BuiltinIn(buildSelection(runtime,bindings, *filter.arg1, plan, registers), args, strings, false);
                                                }
        case QueryGraph::Filter::Builtin_xsddecimal:
                                                return new Selection::BuiltinXSD(buildSelection(runtime,bindings, *filter.arg1, plan, registers));
        case QueryGraph::Filter::Builtin_notexists: {
                                                        std::shared_ptr<QueryGraph> query = filter.subquery;
                                                        std::shared_ptr<QueryGraph::SubQuery> pattern = filter.subpattern;
                                                        if (!query && !pattern) {
                                                            throw;
                                                        }

                                                        //Create operator tree for the subquery
                                                        Operator *tree = NULL;
                                                        std::vector<Register *> regsToLoad;
                                                        std::vector<Register *> regsToCheck;
                                                        if (query) {
                                                            vector<Register*> output;
                                                            tree = CodeGen::translateIntern(runtime, *query.get(), plan, output, registers);
                                                            unsigned i = 0;
                                                            for(auto prj = query->projectionBegin(); prj != query->projectionEnd(); ++prj) {
                                                                if (bindings.count(*prj)) {
                                                                    regsToLoad.push_back(output[i]);
                                                                    regsToCheck.push_back(bindings.find(*prj)->second);
                                                                }
                                                                i++;
                                                            }
                                                        } else {
                                                            map<unsigned, Register*> context, filterBindings;
                                                            std::set<unsigned> projection;
                                                            CodeGen::collectVariables(projection, plan);
                                                            tree = translatePlan(runtime, context, projection, filterBindings, registers, plan);
                                                            for(auto v : projection) {
                                                                if (bindings.count(v)) {
                                                                    regsToCheck.push_back(bindings.find(v)->second);
                                                                    regsToLoad.push_back(filterBindings.find(v)->second);
                                                                }
                                                            }
                                                        }

                                                        return new Selection::BuiltinNotExists(tree, runtime, regsToLoad, regsToCheck);
                                                    }
        case QueryGraph::Filter::Builtin_aggr:
                                                    if (bindings.count(filter.id))
                                                        return new Selection::AggrFunction((*bindings.find(filter.id)).second);
                                                    else
                                                        return new Selection::Null();
    }
    throw; // Cannot happen
}
//---------------------------------------------------------------------------
static Operator* translateGroupBy(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan) {
    Operator* tree = translatePlan(runtime, context, projection, bindings,
            registers, plan->left);

    vector<unsigned> regs;
    //Get the variables
    if (plan->right) {
        const std::vector<unsigned>& groupByVars =
            *reinterpret_cast<const std::vector<unsigned>*>(plan->right);
        for (auto v : groupByVars) {
            if (bindings.count(v)) {
                regs.push_back(v);
            } else {
                // LOG(ERRORL) << "Variable not found";
                // throw 10;
                // ignore, can happen!
            }
        }
    }

    Operator *result = new GroupBy(tree, bindings, regs, plan->opArg == 1,
            tree->getExpectedOutputCardinality());
    return result;
}
//---------------------------------------------------------------------------
static Operator* translateAggregates(Runtime& runtime,
        const map<unsigned, Register*>& context,
        const set<unsigned>& projection,
        map<unsigned, Register*>& bindings,
        const map<const QueryGraph::Node*, unsigned>& registers,
        Plan* plan) {
    QueryGraph *q = (QueryGraph*)plan->right;
    const AggregateHandler &hdl = q->c_getAggredateHandler();
    const auto groupkeys = q->getGroupBy();
    auto vars = hdl.getInputOutputVars();
    set<unsigned> newprojection = projection;
    //Add input variables that are needed for the aggregated variables as projections
    for(auto v : vars.first) {
        newprojection.insert(v);
    }
    //Create bindings for the output variables
    uint64_t slot = 0;
    for(auto v : vars.second) {
        Register* reg = runtime.getRegister((*registers.find(reinterpret_cast<const QueryGraph::Node*>(&hdl))).second + slot);
        bindings[v] = reg;
        slot++;
    }

    Operator* tree = translatePlan(runtime, context, newprojection, bindings,
            registers, plan->left);
    Operator *result = new AggrFunctions(runtime.getDatabase(),
            tree, bindings, hdl,
            groupkeys,
            tree->getExpectedOutputCardinality());
    return result;
}
//---------------------------------------------------------------------------
static Operator* translateHaving(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan) {
    FilterArgs& filterArgs =  *reinterpret_cast<FilterArgs*>(plan->right);
    const QueryGraph::Filter& filter = *filterArgs.filter;

    // Collect all variables
    set<unsigned> filterVariables;
    collectVariables(filterVariables, filter);

    // Build the input trees
    set<unsigned> newProjection = projection;
    for (set<unsigned>::const_iterator iter = filterVariables.begin(),
            limit = filterVariables.end(); iter != limit; ++iter)
        newProjection.insert(*iter);

    Operator* tree = translatePlan(runtime, context, newProjection,
            bindings, registers, plan->left);
    Operator* result = 0;
    if (!result) {
        result = new Selection(tree, runtime,
                buildSelection(runtime, bindings, filter, filterArgs.plan,
                    registers), plan->cardinality);
    }

    // Cleanup the binding
    for (set<unsigned>::const_iterator iter = filterVariables.begin(),
            limit = filterVariables.end(); iter != limit; ++iter)
        if (!projection.count(*iter))
            bindings.erase(*iter);

    return result;
}
//---------------------------------------------------------------------------
static Operator* translateFilter(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
    // Translate a filter into an operator tree
{
    //const QueryGraph::Filter& filter = *reinterpret_cast<QueryGraph::Filter*>(plan->right);
    FilterArgs& filterArgs =  *reinterpret_cast<FilterArgs*>(plan->right);
    const QueryGraph::Filter& filter = *filterArgs.filter;

    // Collect all variables
    set<unsigned> filterVariables;
    collectVariables(filterVariables, filter);

    // Build the input trees
    set<unsigned> newProjection = projection;
    for (set<unsigned>::const_iterator iter = filterVariables.begin(), limit = filterVariables.end(); iter != limit; ++iter)
        newProjection.insert(*iter);
    Operator* tree = translatePlan(runtime, context, newProjection, bindings, registers, plan->left);

    // Build the operator, try special cases first
    Operator* result = 0;
    if (((filter.type == QueryGraph::Filter::Equal) || (filter.type == QueryGraph::Filter::NotEqual)) && (filter.arg1->type == QueryGraph::Filter::Variable)) {
        if (((filter.arg2->type == QueryGraph::Filter::Literal) || (filter.arg2->type == QueryGraph::Filter::IRI)) && (bindings.count(filter.arg1->id))) {
            vector<uint64_t> values;
            values.push_back(filter.arg2->id);
            result = new Filter(tree, bindings[filter.arg1->id], values, filter.type == QueryGraph::Filter::NotEqual, plan->cardinality);
        }
    }
    if ((!result) && ((filter.type == QueryGraph::Filter::Equal) || (filter.type == QueryGraph::Filter::NotEqual)) && (filter.arg2->type == QueryGraph::Filter::Variable)) {
        if (((filter.arg1->type == QueryGraph::Filter::Literal) || (filter.arg1->type == QueryGraph::Filter::IRI)) && (bindings.count(filter.arg2->id))) {
            vector<uint64_t> values;
            values.push_back(filter.arg1->id);
            result = new Filter(tree, bindings[filter.arg2->id], values, filter.type == QueryGraph::Filter::NotEqual, plan->cardinality);
        }
    }
    if ((!result) && (filter.type == QueryGraph::Filter::Builtin_in) && (filter.arg1->type == QueryGraph::Filter::Variable) && (bindings.count(filter.arg1->id))) {
        vector<uint64_t> values;
        bool valid = true;
        if (filter.arg2) {
            const QueryGraph::Filter* iter = filter.arg2;
            for (; iter; iter = iter->arg2) {
                assert(iter->type == QueryGraph::Filter::ArgumentList);
                if ((iter->arg1->type != QueryGraph::Filter::Literal) && (iter->arg1->type != QueryGraph::Filter::IRI)) {
                    valid = false;
                    break;
                }
                values.push_back(iter->arg1->id);
            }
        }
        if (valid) {
            result = new Filter(tree, bindings[filter.arg1->id], values, false, plan->cardinality);
        }
    }
    if (!result) {
        result = new Selection(tree, runtime, buildSelection(runtime, bindings, filter, filterArgs.plan, registers), plan->cardinality);
    }

    // Cleanup the binding
    for (set<unsigned>::const_iterator iter = filterVariables.begin(), limit = filterVariables.end(); iter != limit; ++iter)
        if (!projection.count(*iter))
            bindings.erase(*iter);

    return result;
}
//---------------------------------------------------------------------------
static Operator* translateSubselect(Runtime& runtime, /*const map<unsigned, Register*>& context,*/ const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)

{

    vector<Register*> outputFromTheSubquery;
    QueryGraph* query = (QueryGraph*)plan->right;
    Operator* tree = CodeGen::translateIntern(runtime, *query, plan->left,
            outputFromTheSubquery, registers);
    if (!tree) return 0;

    // And add the output generation
    DuplLimit::DuplicateHandling duplicateHandling = DuplLimit::Duplicates;
    switch (query->getDuplicateHandling()) {
        case QueryGraph::NoDuplicates:
            duplicateHandling = DuplLimit::NoDuplicates;
            break;
        default:
            break;
    }

    //*** Setup the registers ***
    int i = 0;
    for (QueryGraph::projection_iterator itr = query->projectionBegin();
            itr != query->projectionEnd(); ++itr) {
        bindings.insert(make_pair(*itr, outputFromTheSubquery[i++]));
    }

    tree = new DuplLimit(tree, outputFromTheSubquery, duplicateHandling,
            query->getLimit(), query->getOffset());
    return tree;

}
//---------------------------------------------------------------------------
static Operator* translateMinus(Runtime& runtime,const map<unsigned, Register*>& context,
        const set<unsigned>& projection, map<unsigned, Register*>& bindings,
        const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)

{
    if (!plan->right || !plan->right->subquery) {
        throw 1;
    }

    std::set<unsigned> variablesMainPlan;
    CodeGen::collectVariables(variablesMainPlan, plan->left);
    std::set<unsigned> minusVariables;
    CodeGen::collectVariables(minusVariables, plan->right);
    //Find the variables that should be checked
    std::set<unsigned> varsToCheck;
    for(auto varId : minusVariables) {
        if (variablesMainPlan.count(varId)) {
            varsToCheck.insert(varId);
        }
    }

    //Make sure that all the common variables are also projected in the main tree
    auto newprojection = projection;
    for(auto v : varsToCheck) newprojection.insert(v);

    //Convert main query
    Operator* mainTree = translatePlan(runtime,
            context,
            newprojection,
            bindings, registers,
            plan->left);

    map<unsigned, Register*> minusBindings;
    Operator* minusTree = translatePlan(runtime,
            context,
            varsToCheck,
            minusBindings,
            registers,
            plan->right);

    //Create pairs of registers to check
    std::vector<std::pair<Register*, Register*>> regsToCheck;
    for (auto v : varsToCheck) {
        regsToCheck.push_back(make_pair(bindings.find(v)->second,
                    minusBindings.find(v)->second));
    }

    Operator *result = new Minus(mainTree, minusTree, regsToCheck,
            plan->left->cardinality);
    return result;

}
//---------------------------------------------------------------------------
static Operator* translateUnion(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
    // Translate a union into an operator tree
{
    // Collect the parts
    vector<Plan*> parts;
    while (true) {
        parts.push_back(plan->left);
        if (!(plan->right) || plan->right->op != Plan::Union) {
            //J- Not sure why this second operator should be added. I keep it not to break anything
            if (plan->right)
                parts.push_back(plan->right);
            break;
        } else plan = plan->right;
    }

    // Translate the parts of the union
    vector<map<unsigned, Register*> > subBindings;
    vector<Operator*> trees;
    subBindings.resize(parts.size());
    trees.resize(parts.size());
    for (unsigned index = 0; index < parts.size(); index++)
        trees[index] = translatePlan(runtime, context, projection, subBindings[index], registers, parts[index]);

    // Collect all bindings
    for (vector<map<unsigned, Register*> >::const_iterator iter = subBindings.begin(), limit = subBindings.end(); iter != limit; ++iter)
        for (map<unsigned, Register*>::const_iterator iter2 = (*iter).begin(), limit2 = (*iter).end(); iter2 != limit2; ++iter2)
            if (!bindings.count((*iter2).first))
                bindings[(*iter2).first] = (*iter2).second;

    // Construct the mappings and initializations
    vector<vector<Register*> > mappings, initializations;
    mappings.resize(parts.size());
    initializations.resize(parts.size());
    for (unsigned index = 0; index < subBindings.size(); index++) {
        for (map<unsigned, Register*>::const_iterator iter = subBindings[index].begin(), limit = subBindings[index].end(); iter != limit; ++iter)
            if (bindings[(*iter).first] != (*iter).second) {
                mappings[index].push_back((*iter).second);
                mappings[index].push_back(bindings[(*iter).first]);
            }
        for (map<unsigned, Register*>::const_iterator iter = bindings.begin(), limit = bindings.end(); iter != limit; ++iter)
            if (!subBindings[index].count((*iter).first))
                initializations[index].push_back((*iter).second);
    }

    // Build the operator
    Operator* result = new Union(trees, mappings, initializations, plan->cardinality);

    return result;
}
//---------------------------------------------------------------------------
static Operator* translateMergeUnion(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
    // Translate a merge union into an operator tree
{
    // Translate the input
    map<unsigned, Register*> leftBinding, rightBinding;
    Operator* left = translatePlan(runtime, context, projection, leftBinding, registers, plan->left);
    Operator* right = translatePlan(runtime, context, projection, rightBinding, registers, plan->right);

    // Collect the binding
    assert(leftBinding.size() == 1);
    assert(rightBinding.size() == 1);
    unsigned resultVar = (*(leftBinding.begin())).first;
    Register* leftReg = (*(leftBinding.begin())).second, *rightReg = (*(rightBinding.begin())).second;
    bindings[resultVar] = leftReg;

    // Build the operator
    Operator* result = new MergeUnion(leftReg, left, leftReg, right, rightReg, plan->cardinality);

    return result;
}
//---------------------------------------------------------------------------
static Operator* translateTableFunction(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
    // Translate a table function into an operator tree
{
    const QueryGraph::TableFunction& function = *reinterpret_cast<QueryGraph::TableFunction*>(plan->right);

    // Build the input trees
    set<unsigned> newProjection = projection;
    for (vector<QueryGraph::TableFunction::Argument>::const_iterator iter = function.input.begin(),
            limit = function.input.end(); iter != limit; ++iter)
        if (~(*iter).id)
            newProjection.insert((*iter).id);

    Operator* tree = translatePlan(runtime, context, newProjection, bindings,
            registers, plan->left);

    vector<TableFunction::FunctionArgument> input;
    for (vector<QueryGraph::TableFunction::Argument>::const_iterator iter = function.input.begin(),
            limit = function.input.end(); iter != limit; ++iter) {
        TableFunction::FunctionArgument arg;
        if (~(*iter).id) {
            arg.reg = bindings[(*iter).id];
        } else {
            arg.reg = 0;
            arg.value = (*iter).value;
            arg.idvalue = (*iter).idvalue;
        }
        input.push_back(arg);
    }

    // Examine output registers
    vector<Register*> output;
    unsigned slot = 0;
    for (vector<unsigned>::const_iterator iter = function.output.begin(),
            limit = function.output.end(); iter != limit; ++iter, ++slot) {
        Register* reg = runtime.getRegister((*registers.find(reinterpret_cast<const QueryGraph::Node*>(&function))).second + slot);
        output.push_back(reg);
        if (projection.count(*iter))
            bindings[*iter] = reg;
    }

    // Build the operator
    Operator* result;
    if (function.name == string("BIND")) {
        //Convert the input in an array of assignment arguments
        std::vector<Assignment::AssignmentArgument> assInput;
        for (std::vector<TableFunction::FunctionArgument>::const_iterator iter = input.begin();
                iter != input.end(); ++iter) {
            //Here there should be only one variable/literal in input
            Assignment::AssignmentArgument arg;
            arg.reg = iter->reg;
            if (!arg.reg) {
                arg.defaultValue = iter->idvalue;
            } else {
                arg.defaultValue = 0;
            }
            assInput.push_back(arg);
        }
        result = new Assignment(tree, assInput, output, plan->cardinality);
    } else {
        result = new TableFunction(tree, runtime, 0 /* XXX */, function.name, input, output, plan->cardinality);
    }

    // Cleanup the binding
    for (vector<QueryGraph::TableFunction::Argument>::const_iterator iter = function.input.begin(), limit = function.input.end(); iter != limit; ++iter)
        if ((~(*iter).id) && (!projection.count((*iter).id)))
            bindings.erase((*iter).id);

    return result;
}
//---------------------------------------------------------------------------
static Operator* translatePlan(Runtime& runtime, const map<unsigned, Register*>& context, const set<unsigned>& projection, map<unsigned, Register*>& bindings, const map<const QueryGraph::Node*, unsigned>& registers, Plan* plan)
    // Translate a plan into an operator tree
{
    Operator* result = 0;
    switch (plan->op) {
        case Plan::IndexScan:
            result = translateIndexScan(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::AggregatedIndexScan:
            result = translateAggregatedIndexScan(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::FullyAggregatedIndexScan:
            result = translateFullyAggregatedIndexScan(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::NestedLoopJoin:
            result = translateNestedLoopJoin(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::MergeJoin:
            result = translateMergeJoin(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::HashJoin:
            result = translateHashJoin(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::CartProd:
            result = translateCartProd(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::HashGroupify:
            result = translateHashGroupify(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::Filter:
            result = translateFilter(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::Union:
            result = translateUnion(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::Subselect:
            result = translateSubselect(runtime, /* context,*/ projection, bindings, registers, plan);
            break;
        case Plan::Minus:
            result = translateMinus(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::ValuesScan:
            result = translateValuesScan(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::MergeUnion:
            result = translateMergeUnion(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::TableFunction:
            result = translateTableFunction(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::Singleton:
            result = new SingletonScan();
            break;
        case Plan::GroupBy:
            result = translateGroupBy(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::Having:
            result = translateHaving(runtime, context, projection, bindings, registers, plan);
            break;
        case Plan::Aggregates:
            result = translateAggregates(runtime, context, projection, bindings, registers, plan);
            break;
    }
    return result;
}
//---------------------------------------------------------------------------
static unsigned allocateRegisters(map<const QueryGraph::Node*, unsigned>& registers, map<unsigned, set<unsigned> >& registerClasses, const QueryGraph& query, unsigned id);
static unsigned allocateRegisters(map<const QueryGraph::Node*, unsigned>& registers, map<unsigned, set<unsigned> >& registerClasses, const QueryGraph::SubQuery& query, unsigned id)
    // Allocate registers
{
    for (vector<QueryGraph::Node>::const_iterator iter = query.nodes.begin(), limit = query.nodes.end(); iter != limit; ++iter) {
        const QueryGraph::Node& node = *iter;
        registers[&node] = id;
        if (!node.constSubject)
            registerClasses[node.subject].insert(id + 0);
        if (!node.constPredicate)
            registerClasses[node.predicate].insert(id + 1);
        if (!node.constObject)
            registerClasses[node.object].insert(id + 2);
        id += 3;
    }

    for (vector<QueryGraph::SubQuery>::const_iterator iter = query.optional.begin(), limit = query.optional.end(); iter != limit; ++iter)
        id = allocateRegisters(registers, registerClasses, (*iter), id);

    for (vector<vector<QueryGraph::SubQuery> >::const_iterator iter = query.unions.begin(), limit = query.unions.end(); iter != limit; ++iter)
        for (vector<QueryGraph::SubQuery>::const_iterator iter2 = (*iter).begin(), limit2 = (*iter).end(); iter2 != limit2; ++iter2)
            id = allocateRegisters(registers, registerClasses, (*iter2), id);

    for (vector<QueryGraph::TableFunction>::const_iterator iter = query.tableFunctions.begin(), limit = query.tableFunctions.end(); iter != limit; ++iter) {
        registers[reinterpret_cast<const QueryGraph::Node*>(&(*iter))] = id;
        unsigned slot = 0;
        for (vector<unsigned>::const_iterator iter2 = (*iter).output.begin(), limit2 = (*iter).output.end(); iter2 != limit2; ++iter2, ++slot)
            registerClasses[*iter2].insert(id + slot);
        id += (*iter).output.size();
    }

    for (std::vector<std::shared_ptr<QueryGraph>>::const_iterator itr = query.subqueries.begin();
            itr != query.subqueries.end(); ++itr) {
        QueryGraph* subq = itr->get();
        // id = allocateRegisters(registers, registerClasses, *subq->getQuery(), id);
        id = allocateRegisters(registers, registerClasses, *subq, id);
    }

    for (std::vector<std::shared_ptr<QueryGraph>>::const_iterator itr = query.minuses.begin();
            itr != query.minuses.end(); ++itr) {
        QueryGraph* subq = itr->get();
        // id = allocateRegisters(registers, registerClasses, subq->getQuery(), id);
        id = allocateRegisters(registers, registerClasses, *subq, id);
    }

    for (auto itr = query.valueNodes.begin();
            itr != query.valueNodes.end(); ++itr) {
        QueryGraph::Node *n = (QueryGraph::Node*)&(*itr);
        registers[n] = id;
        for (auto v : itr->variables) {
            registerClasses[v].insert(id++);
        }
    }
    for(auto itr = query.filters.begin(); itr != query.filters.end(); ++itr) {
        if (itr->subpattern != NULL) {
            id = allocateRegisters(registers, registerClasses, *itr->subpattern.get(), id);
        }
        if (itr->subquery != NULL) {
            id = allocateRegisters(registers, registerClasses, *itr->subquery.get(), id);
        }
    }
    return id;
}
//---------------------------------------------------------------------------
static unsigned allocateRegisters(map<const QueryGraph::Node*, unsigned>& registers, map<unsigned, set<unsigned> >& registerClasses, const QueryGraph& query, unsigned id)
{
    id = allocateRegisters(registers, registerClasses, query.getQuery(), id);
    //Also process the global functions
    for (vector<QueryGraph::TableFunction>::const_iterator iter = query.c_getGlobalAssignments().begin(), limit = query.c_getGlobalAssignments().end(); iter != limit; ++iter) {
        registers[reinterpret_cast<const QueryGraph::Node*>(&(*iter))] = id;
        unsigned slot = 0;
        for (vector<unsigned>::const_iterator iter2 = (*iter).output.begin(), limit2 = (*iter).output.end(); iter2 != limit2; ++iter2, ++slot)
            registerClasses[*iter2].insert(id + slot);
        id += (*iter).output.size();
    }
    if (!query.c_getAggredateHandler().empty()) {
        registers[reinterpret_cast<const QueryGraph::Node*>(&(query.c_getAggredateHandler()))] = id;
        //Add the classes
        auto vars = query.c_getAggredateHandler().getInputOutputVars();
        id +=vars.second.size();
    }
    return id;
}
//---------------------------------------------------------------------------
Operator* CodeGen::translateIntern(Runtime& runtime, const QueryGraph& query, Plan* plan, vector<Register*>& output, const map<const QueryGraph::Node*, unsigned> &registers)
    // Perform a naive translation of a query into an operator tree without output generation
{
    // Build the operator tree
    Operator* tree;
    if (query.knownEmpty()) {
        tree = new EmptyScan();
    } else if (!plan) {
        tree = new SingletonScan();
    } else {
        // Construct the projection
        set<unsigned> projection;
        for (QueryGraph::projection_iterator iter = query.projectionBegin(), limit = query.projectionEnd(); iter != limit; ++iter) {
            projection.insert(*iter);
        }

        for (QueryGraph::order_iterator iter = query.orderBegin(), limit = query.orderEnd(); iter != limit; ++iter)
            if (~(*iter).id)
                projection.insert((*iter).id);

        // And build the tree
        map<unsigned, Register*> context, bindings;
        tree = translatePlan(runtime, context, projection, bindings, registers, plan);

        // Sort if necessary
        if (query.orderBegin() != query.orderEnd()) {
            vector<Register*> regs;
            vector<pair<Register*, bool> > order;
            for (set<unsigned>::const_iterator iter = projection.begin(), limit = projection.end(); iter != limit; ++iter)
                regs.push_back(bindings[*iter]);
            for (QueryGraph::order_iterator iter = query.orderBegin(), limit = query.orderEnd(); iter != limit; ++iter)
                if (~(*iter).id)
                    order.push_back(pair<Register*, bool>(bindings[(*iter).id], (*iter).descending));
                else
                    order.push_back(pair<Register*, bool>(0, (*iter).descending));
            tree = new Sort(runtime.getDatabase(), tree, regs, order, tree->getExpectedOutputCardinality());
        }

        // Remember the output registers
        for (QueryGraph::projection_iterator iter = query.projectionBegin(), limit = query.projectionEnd(); iter != limit; ++iter)
            if (bindings.count(*iter))
                output.push_back(bindings[*iter]);
            else
                output.push_back(runtime.getRegister(runtime.getRegisterCount() - 1));
    }

    return tree;
}
//---------------------------------------------------------------------------

void CodeGen::prepareRuntime(Runtime &runtime,
        const QueryGraph& query,
        std::map<const QueryGraph::Node*, unsigned> &registers)
{
    map<unsigned, set<unsigned> > registerClasses;
    unsigned registerCount = allocateRegisters(registers, registerClasses, query, 0);
    runtime.allocateRegisters(registerCount + 1);
    // Prepare domain information for join attributes
    {
        // Count the required number of domains
        unsigned domainCount = 0;
        for (map<unsigned, set<unsigned> >::const_iterator iter = registerClasses.begin(), limit = registerClasses.end(); iter != limit; ++iter) {
            // No join attribute?
            if ((*iter).second.size() < 2)
                continue;
            // We have a new domain
            domainCount++;
        }
        runtime.allocateDomainDescriptions(domainCount);

        // And assign registers to domains
        domainCount = 0;
        for (map<unsigned, set<unsigned> >::const_iterator iter = registerClasses.begin(), limit = registerClasses.end(); iter != limit; ++iter) {
            // No join attribute?
            if ((*iter).second.size() < 2)
                continue;
            // Lookup the register addresses
            PotentialDomainDescription* domain = runtime.getDomainDescription(domainCount++);
            for (set<unsigned>::const_iterator iter2 = (*iter).second.begin(), limit2 = (*iter).second.end(); iter2 != limit2; ++iter2)
                runtime.getRegister(*iter2)->domain = domain;
        }
    }
}
//---------------------------------------------------------------------------
Operator* CodeGen::translate(Runtime& runtime, const QueryGraph& query, Plan* plan, bool silent)
    // Perform a naive translation of a query into an operator tree
{
    std::map<const QueryGraph::Node*, unsigned> registers;
    prepareRuntime(runtime, query, registers);

    // Build the tree itself
    vector<Register*> output;
    Operator* tree = translateIntern(runtime, query, plan, output, registers);
    if (!tree) return 0;

    // And add the output generation
    ResultsPrinter::DuplicateHandling duplicateHandling = ResultsPrinter::ExpandDuplicates;
    switch (query.getDuplicateHandling()) {
        case QueryGraph::AllDuplicates:
            duplicateHandling = ResultsPrinter::ExpandDuplicates;
            break;
        case QueryGraph::CountDuplicates:
            duplicateHandling = ResultsPrinter::CountDuplicates;
            break;
        case QueryGraph::ReducedDuplicates:
            duplicateHandling = ResultsPrinter::ReduceDuplicates;
            break;
        case QueryGraph::NoDuplicates:
            duplicateHandling = ResultsPrinter::ReduceDuplicates;
            break;
        case QueryGraph::ShowDuplicates:
            duplicateHandling = ResultsPrinter::ShowDuplicates;
            break;
    }
    tree = new ResultsPrinter(runtime, tree, output, duplicateHandling, query.getLimit(), query.getOffset(), silent);

    return tree;
}
//---------------------------------------------------------------------------
void CodeGen::collectVariables(set<unsigned>& variables, Plan* plan)
    // Collect all variables contained in a plan
{
    ::collectVariables(map<unsigned, Register*>(), variables, plan);
}
//---------------------------------------------------------------------------
