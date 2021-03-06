#include <Storages/MergeTree/MergeTreeIndexConditionBloomFilter.h>
#include <Interpreters/misc.h>
#include <Interpreters/BloomFilterHash.h>
#include <Common/HashTable/ClearableHashMap.h>
#include <Storages/MergeTree/RPNBuilder.h>
#include <Storages/MergeTree/MergeTreeIndexGranuleBloomFilter.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeTuple.h>
#include <Columns/ColumnConst.h>
#include <ext/bit_cast.h>
#include <Parsers/ASTSubquery.h>
#include <Parsers/ASTIdentifier.h>
#include <Columns/ColumnTuple.h>
#include <Interpreters/castColumn.h>
#include <Interpreters/convertFieldToType.h>


namespace DB
{

namespace
{

PreparedSetKey getPreparedSetKey(const ASTPtr & node, const DataTypePtr & data_type)
{
    /// If the data type is tuple, let's try unbox once
    if (node->as<ASTSubquery>() || node->as<ASTIdentifier>())
        return PreparedSetKey::forSubquery(*node);

    if (const auto * date_type_tuple = typeid_cast<const DataTypeTuple *>(&*data_type))
        return PreparedSetKey::forLiteral(*node, date_type_tuple->getElements());

    return PreparedSetKey::forLiteral(*node, DataTypes(1, data_type));
}

ColumnWithTypeAndName getPreparedSetInfo(const SetPtr & prepared_set)
{
    if (prepared_set->getDataTypes().size() == 1)
        return {prepared_set->getSetElements()[0], prepared_set->getDataTypes()[0], "dummy"};

    return {ColumnTuple::create(prepared_set->getSetElements()), std::make_shared<DataTypeTuple>(prepared_set->getDataTypes()), "dummy"};
}

bool maybeTrueOnBloomFilter(const IColumn * hash_column, const BloomFilterPtr & bloom_filter, size_t hash_functions)
{
    const auto const_column = typeid_cast<const ColumnConst *>(hash_column);
    const auto non_const_column = typeid_cast<const ColumnUInt64 *>(hash_column);

    if (!const_column && !non_const_column)
        throw Exception("LOGICAL ERROR: hash column must be Const Column or UInt64 Column.", ErrorCodes::LOGICAL_ERROR);

    if (const_column)
    {
        for (size_t index = 0; index < hash_functions; ++index)
            if (!bloom_filter->findHashWithSeed(const_column->getValue<UInt64>(), BloomFilterHash::bf_hash_seed[index]))
                return false;
        return true;
    }
    else
    {
        bool missing_rows = true;
        const ColumnUInt64::Container & data = non_const_column->getData();

        for (size_t index = 0, size = data.size(); missing_rows && index < size; ++index)
        {
            bool match_row = true;
            for (size_t hash_index = 0; match_row && hash_index < hash_functions; ++hash_index)
                match_row = bloom_filter->findHashWithSeed(data[index], BloomFilterHash::bf_hash_seed[hash_index]);

            missing_rows = !match_row;
        }

        return !missing_rows;
    }
}

}

MergeTreeIndexConditionBloomFilter::MergeTreeIndexConditionBloomFilter(
    const SelectQueryInfo & info_, const Context & context_, const Block & header_, size_t hash_functions_)
    : header(header_), context(context_), query_info(info_), hash_functions(hash_functions_)
{
    auto atomFromAST = [this](auto & node, auto &, auto & constants, auto & out) { return traverseAtomAST(node, constants, out); };
    rpn = std::move(RPNBuilder<RPNElement>(info_, context, atomFromAST).extractRPN());
}

bool MergeTreeIndexConditionBloomFilter::alwaysUnknownOrTrue() const
{
    std::vector<bool> rpn_stack;

    for (const auto & element : rpn)
    {
        if (element.function == RPNElement::FUNCTION_UNKNOWN
            || element.function == RPNElement::ALWAYS_TRUE)
        {
            rpn_stack.push_back(true);
        }
        else if (element.function == RPNElement::FUNCTION_EQUALS
                 || element.function == RPNElement::FUNCTION_NOT_EQUALS
                 || element.function == RPNElement::FUNCTION_HAS
                 || element.function == RPNElement::FUNCTION_IN
                 || element.function == RPNElement::FUNCTION_NOT_IN
                 || element.function == RPNElement::ALWAYS_FALSE)
        {
            rpn_stack.push_back(false);
        }
        else if (element.function == RPNElement::FUNCTION_NOT)
        {
            // do nothing
        }
        else if (element.function == RPNElement::FUNCTION_AND)
        {
            auto arg1 = rpn_stack.back();
            rpn_stack.pop_back();
            auto arg2 = rpn_stack.back();
            rpn_stack.back() = arg1 && arg2;
        }
        else if (element.function == RPNElement::FUNCTION_OR)
        {
            auto arg1 = rpn_stack.back();
            rpn_stack.pop_back();
            auto arg2 = rpn_stack.back();
            rpn_stack.back() = arg1 || arg2;
        }
        else
            throw Exception("Unexpected function type in KeyCondition::RPNElement", ErrorCodes::LOGICAL_ERROR);
    }

    return rpn_stack[0];
}

bool MergeTreeIndexConditionBloomFilter::mayBeTrueOnGranule(const MergeTreeIndexGranuleBloomFilter * granule) const
{
    std::vector<BoolMask> rpn_stack;
    const auto & filters = granule->getFilters();

    for (const auto & element : rpn)
    {
        if (element.function == RPNElement::FUNCTION_UNKNOWN)
        {
            rpn_stack.emplace_back(true, true);
        }
        else if (element.function == RPNElement::FUNCTION_IN
            || element.function == RPNElement::FUNCTION_NOT_IN
            || element.function == RPNElement::FUNCTION_EQUALS
            || element.function == RPNElement::FUNCTION_NOT_EQUALS
            || element.function == RPNElement::FUNCTION_HAS)
        {
            bool match_rows = true;
            const auto & predicate = element.predicate;
            for (size_t index = 0; match_rows && index < predicate.size(); ++index)
            {
                const auto & query_index_hash = predicate[index];
                const auto & filter = filters[query_index_hash.first];
                const ColumnPtr & hash_column = query_index_hash.second;
                match_rows = maybeTrueOnBloomFilter(&*hash_column, filter, hash_functions);
            }

            rpn_stack.emplace_back(match_rows, !match_rows);
            if (element.function == RPNElement::FUNCTION_NOT_EQUALS || element.function == RPNElement::FUNCTION_NOT_IN)
                rpn_stack.back() = !rpn_stack.back();
        }
        else if (element.function == RPNElement::FUNCTION_NOT)
        {
            rpn_stack.back() = !rpn_stack.back();
        }
        else if (element.function == RPNElement::FUNCTION_OR)
        {
            auto arg1 = rpn_stack.back();
            rpn_stack.pop_back();
            auto arg2 = rpn_stack.back();
            rpn_stack.back() = arg1 | arg2;
        }
        else if (element.function == RPNElement::FUNCTION_AND)
        {
            auto arg1 = rpn_stack.back();
            rpn_stack.pop_back();
            auto arg2 = rpn_stack.back();
            rpn_stack.back() = arg1 & arg2;
        }
        else if (element.function == RPNElement::ALWAYS_TRUE)
        {
            rpn_stack.emplace_back(true, false);
        }
        else if (element.function == RPNElement::ALWAYS_FALSE)
        {
            rpn_stack.emplace_back(false, true);
        }
        else
            throw Exception("Unexpected function type in KeyCondition::RPNElement", ErrorCodes::LOGICAL_ERROR);
    }

    if (rpn_stack.size() != 1)
        throw Exception("Unexpected stack size in KeyCondition::mayBeTrueInRange", ErrorCodes::LOGICAL_ERROR);

    return rpn_stack[0].can_be_true;
}

bool MergeTreeIndexConditionBloomFilter::traverseAtomAST(const ASTPtr & node, Block & block_with_constants, RPNElement & out)
{
    {
        Field const_value;
        DataTypePtr const_type;
        if (KeyCondition::getConstant(node, block_with_constants, const_value, const_type))
        {
            if (const_value.getType() == Field::Types::UInt64 || const_value.getType() == Field::Types::Int64 ||
                const_value.getType() == Field::Types::Float64)
            {
                /// Zero in all types is represented in memory the same way as in UInt64.
                out.function = const_value.get<UInt64>() ? RPNElement::ALWAYS_TRUE : RPNElement::ALWAYS_FALSE;
                return true;
            }
        }
    }

    if (const auto * function = node->as<ASTFunction>())
    {
        const ASTs & arguments = function->arguments->children;

        if (arguments.size() != 2)
            return false;

        if (functionIsInOrGlobalInOperator(function->name))
        {
            if (const auto & prepared_set = getPreparedSet(arguments[1]))
                return traverseASTIn(function->name, arguments[0], prepared_set, out);
        }
        else if (function->name == "equals" || function->name  == "notEquals" || function->name == "has")
        {
            Field const_value;
            DataTypePtr const_type;
            if (KeyCondition::getConstant(arguments[1], block_with_constants, const_value, const_type))
                return traverseASTEquals(function->name, arguments[0], const_type, const_value, out);
            else if (KeyCondition::getConstant(arguments[0], block_with_constants, const_value, const_type))
                return traverseASTEquals(function->name, arguments[1], const_type, const_value, out);
        }
    }

    return false;
}

bool MergeTreeIndexConditionBloomFilter::traverseASTIn(
    const String & function_name, const ASTPtr & key_ast, const SetPtr & prepared_set, RPNElement & out)
{
    const auto & prepared_info = getPreparedSetInfo(prepared_set);
    return traverseASTIn(function_name, key_ast, prepared_info.type, prepared_info.column, out);
}

bool MergeTreeIndexConditionBloomFilter::traverseASTIn(
    const String & function_name, const ASTPtr & key_ast, const DataTypePtr & type, const ColumnPtr & column, RPNElement & out)
{
    if (header.has(key_ast->getColumnName()))
    {
        size_t row_size = column->size();
        size_t position = header.getPositionByName(key_ast->getColumnName());
        const DataTypePtr & index_type = header.getByPosition(position).type;
        const auto & converted_column = castColumn(ColumnWithTypeAndName{column, type, ""}, index_type, context);
        out.predicate.emplace_back(std::make_pair(position, BloomFilterHash::hashWithColumn(index_type, converted_column, 0, row_size)));

        if (function_name == "in"  || function_name == "globalIn")
            out.function = RPNElement::FUNCTION_IN;

        if (function_name == "notIn"  || function_name == "globalNotIn")
            out.function = RPNElement::FUNCTION_NOT_IN;

        return true;
    }

    if (const auto * function = key_ast->as<ASTFunction>())
    {
        WhichDataType which(type);

        if (which.isTuple() && function->name == "tuple")
        {
            const auto & tuple_column = typeid_cast<const ColumnTuple *>(column.get());
            const auto & tuple_data_type = typeid_cast<const DataTypeTuple *>(type.get());
            const ASTs & arguments = typeid_cast<const ASTExpressionList &>(*function->arguments).children;

            if (tuple_data_type->getElements().size() != arguments.size() || tuple_column->getColumns().size() != arguments.size())
                throw Exception("Illegal types of arguments of function " + function_name, ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

            bool match_with_subtype = false;
            const auto & sub_columns = tuple_column->getColumns();
            const auto & sub_data_types = tuple_data_type->getElements();

            for (size_t index = 0; index < arguments.size(); ++index)
                match_with_subtype |= traverseASTIn(function_name, arguments[index], sub_data_types[index], sub_columns[index], out);

            return match_with_subtype;
        }
    }

    return false;
}

bool MergeTreeIndexConditionBloomFilter::traverseASTEquals(
    const String & function_name, const ASTPtr & key_ast, const DataTypePtr & value_type, const Field & value_field, RPNElement & out)
{
    if (header.has(key_ast->getColumnName()))
    {
        size_t position = header.getPositionByName(key_ast->getColumnName());
        const DataTypePtr & index_type = header.getByPosition(position).type;
        const DataTypeArray * array_type = typeid_cast<const DataTypeArray *>(index_type.get());

        if (function_name == "has")
        {
            out.function = RPNElement::FUNCTION_HAS;

            if (!array_type)
                throw Exception("First argument for function has must be an array.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

            Field converted_field = convertFieldToType(value_field, *array_type->getNestedType(), &*value_type);
            out.predicate.emplace_back(std::make_pair(position, BloomFilterHash::hashWithField(&*array_type->getNestedType(), converted_field)));
        }
        else
        {
            if (array_type)
                throw Exception("An array type of bloom_filter supports only has() function.", ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

            out.function = function_name == "equals" ? RPNElement::FUNCTION_EQUALS : RPNElement::FUNCTION_NOT_EQUALS;
            Field converted_field = convertFieldToType(value_field, *index_type, &*value_type);
            out.predicate.emplace_back(std::make_pair(position, BloomFilterHash::hashWithField(&*index_type, converted_field)));
        }

        return true;
    }

    if (const auto * function = key_ast->as<ASTFunction>())
    {
        WhichDataType which(value_type);

        if (which.isTuple() && function->name == "tuple")
        {
            const Tuple & tuple = get<const Tuple &>(value_field);
            const auto value_tuple_data_type = typeid_cast<const DataTypeTuple *>(value_type.get());
            const ASTs & arguments = typeid_cast<const ASTExpressionList &>(*function->arguments).children;

            if (tuple.size() != arguments.size())
                throw Exception("Illegal types of arguments of function " + function_name, ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);

            bool match_with_subtype = false;
            const DataTypes & subtypes = value_tuple_data_type->getElements();

            for (size_t index = 0; index < tuple.size(); ++index)
                match_with_subtype |= traverseASTEquals(function_name, arguments[index], subtypes[index], tuple[index], out);

            return match_with_subtype;
        }
    }

    return false;
}

SetPtr MergeTreeIndexConditionBloomFilter::getPreparedSet(const ASTPtr & node)
{
    if (header.has(node->getColumnName()))
    {
        const auto & column_and_type = header.getByName(node->getColumnName());
        const auto & prepared_set_it = query_info.sets.find(getPreparedSetKey(node, column_and_type.type));

        if (prepared_set_it != query_info.sets.end() && prepared_set_it->second->hasExplicitSetElements())
            return prepared_set_it->second;
    }
    else
    {
        for (const auto & prepared_set_it : query_info.sets)
            if (prepared_set_it.first.ast_hash == node->getTreeHash() && prepared_set_it.second->hasExplicitSetElements())
                return prepared_set_it.second;
    }

    return DB::SetPtr();
}

}
