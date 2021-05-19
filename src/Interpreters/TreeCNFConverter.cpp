#include <Interpreters/TreeCNFConverter.h>
#include <Parsers/IAST.h>
#include <Parsers/ASTFunction.h>
#include <Poco/Logger.h>

namespace DB
{
namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int INCORRECT_QUERY;
}

/// Splits AND(a, b, c) to AND(a, AND(b, c)) for AND/OR
void splitMultiLogic(ASTPtr & node)
{
    auto * func = node->as<ASTFunction>();

    if (func && (func->name == "and" || func->name == "or"))
    {
        if (func->arguments->children.size() < 2)
            throw Exception("Bad logical function", ErrorCodes::INCORRECT_QUERY);

        if (func->arguments->children.size() > 2)
        {
            ASTPtr res = func->arguments->children[0]->clone();
            for (size_t i = 1; i < func->arguments->children.size(); ++i)
            {
                res = makeASTFunction(func->name, res, func->arguments->children[i]->clone());
            }
            node = res;
        }

        auto * new_func = node->as<ASTFunction>();
        for (auto & child : new_func->arguments->children)
            splitMultiLogic(child);
    }
    else if (func && func->name == "not")
    {
        for (auto & child : func->arguments->children)
            splitMultiLogic(child);
    }
}

/// Push NOT to leafs, remove NOT NOT ...
void traversePushNot(ASTPtr & node, bool add_negation)
{
    auto * func = node->as<ASTFunction>();

    if (func && (func->name == "and" || func->name == "or"))
    {
        if (add_negation)
        {
            if (func->arguments->children.size() != 2)
                throw Exception("Bad AND or OR function.", ErrorCodes::LOGICAL_ERROR);
            /// apply De Morgan's Law
            node = makeASTFunction(
                (func->name == "and" ? "or" : "and"),
                func->arguments->children[0]->clone(),
                func->arguments->children[1]->clone());
        }

        auto * new_func = node->as<ASTFunction>();
        for (auto & child : new_func->arguments->children)
            traversePushNot(child, add_negation);
    }
    else if (func && func->name == "not")
    {
        if (func->arguments->children.size() != 1)
            throw Exception("Bad NOT function.", ErrorCodes::INCORRECT_QUERY);
        /// delete NOT
        node = func->arguments->children[0]->clone();

        traversePushNot(node, !add_negation);
    }
    else
    {
        if (add_negation)
            node = makeASTFunction("not", node->clone());
    }
}

/// Push Or inside And (actually pull AND to top)
void traversePushOr(ASTPtr & node)
{
    auto * func = node->as<ASTFunction>();

    if (func && (func->name == "or" || func->name == "and"))
    {
        for (auto & child : func->arguments->children)
            traversePushOr(child);
    }

    if (func && func->name == "or")
    {
        size_t and_node_id = func->arguments->children.size();
        for (size_t i = 0; i < func->arguments->children.size(); ++i)
        {
            auto & child = func->arguments->children[i];
            auto * and_func = child->as<ASTFunction>();
            if (and_func && and_func->name == "and")
            {
                and_node_id = i;
            }
        }
        if (and_node_id == func->arguments->children.size())
            return;
        const size_t other_node_id = 1 - and_node_id;

        const auto * and_func = func->arguments->children[and_node_id]->as<ASTFunction>();
        auto a = func->arguments->children[other_node_id];
        auto b = and_func->arguments->children[0];
        auto c = and_func->arguments->children[1];

        /// apply the distributive law ( a or (b and c) -> (a or b) and (a or c) )
        node = makeASTFunction(
            "and",
            makeASTFunction("or", a->clone(), b),
            makeASTFunction("or", a, c));

        traversePushOr(node);
    }
}

/// transform ast into cnf groups
void traverseCNF(const ASTPtr & node, CNFQuery::AndGroup & and_group, CNFQuery::OrGroup & or_group)
{
    auto * func = node->as<ASTFunction>();
    if (func && func->name == "and")
    {
        for (auto & child : func->arguments->children)
        {
            CNFQuery::OrGroup group;
            traverseCNF(child, and_group, group);
            if (!group.empty())
                and_group.insert(std::move(group));
        }
    }
    else if (func && func->name == "or")
    {
        for (auto & child : func->arguments->children)
        {
            traverseCNF(child, and_group, or_group);
        }
    }
    else if (func && func->name == "not")
    {
        if (func->arguments->children.size() != 1)
            throw Exception("Bad NOT function", ErrorCodes::INCORRECT_QUERY);
        or_group.insert(CNFQuery::AtomicFormula{true, func->arguments->children.front()});
    }
    else
    {
        or_group.insert(CNFQuery::AtomicFormula{false, node});
    }
}

void traverseCNF(const ASTPtr & node, CNFQuery::AndGroup & result)
{
    CNFQuery::OrGroup or_group;
    traverseCNF(node, result, or_group);
    if (!or_group.empty())
        result.insert(or_group);
}

CNFQuery TreeCNFConverter::toCNF(const ASTPtr & query)
{
    auto cnf = query->clone();

    splitMultiLogic(cnf);
    traversePushNot(cnf, false);
    traversePushOr(cnf);
    CNFQuery::AndGroup and_group;
    traverseCNF(cnf, and_group);

    CNFQuery result{std::move(and_group)};

    Poco::Logger::get("TreeCNFConverter").information("Converted to CNF: " + result.dump());
    return result;
}

ASTPtr TreeCNFConverter::fromCNF(const CNFQuery & cnf)
{
    const auto & groups = cnf.getStatements();
    if (groups.empty())
        return nullptr;

    ASTs or_groups;
    for (const auto & group : groups)
    {
        if (group.size() == 1)
        {
            if ((*group.begin()).negative)
                or_groups.push_back(makeASTFunction("not", (*group.begin()).ast->clone()));
            else
                or_groups.push_back((*group.begin()).ast->clone());
        }
        else if (group.size() > 1)
        {
            or_groups.push_back(makeASTFunction("or"));
            auto * func = or_groups.back()->as<ASTFunction>();
            for (const auto & atom : group)
            {
                if ((*group.begin()).negative)
                    func->arguments->children.push_back(makeASTFunction("not", atom.ast->clone()));
                else
                    func->arguments->children.push_back(atom.ast->clone());
            }
        }
    }

    if (or_groups.size() == 1)
        return or_groups.front();

    ASTPtr res = makeASTFunction("and");
    auto * func = res->as<ASTFunction>();
    for (const auto & group : or_groups)
        func->arguments->children.push_back(group);

    return res;
}

void pushPullNotInAtom(CNFQuery::AtomicFormula & atom, const std::map<std::string, std::string> & inverse_relations)
{
    auto * func = atom.ast->as<ASTFunction>();
    if (!func)
        return;
    if (auto it = inverse_relations.find(func->name); it != std::end(inverse_relations))
    {
        /// inverse func
        atom.ast = atom.ast->clone();
        auto * new_func = atom.ast->as<ASTFunction>();
        new_func->name = it->second;
        /// add not
        atom.negative = !atom.negative;
    }
}

void pullNotOut(CNFQuery::AtomicFormula & atom)
{
    static const std::map<std::string, std::string> inverse_relations = {
        {"notEquals", "equals"},
        {"greaterOrEquals", "less"},
        {"greater", "lessOrEquals"},
        {"notIn", "in"},
        {"notLike", "like"},
        {"notEmpty", "empty"},
    };

    pushPullNotInAtom(atom, inverse_relations);
}

void pushNotIn(CNFQuery::AtomicFormula & atom)
{
    if (!atom.negative)
        return;

    static const std::map<std::string, std::string> inverse_relations = {
        {"equals", "notEquals"},
        {"less", "greaterOrEquals"},
        {"lessOrEquals", "greater"},
        {"in", "notIn"},
        {"like", "notLike"},
        {"empty", "notEmpty"},
        {"notEquals", "equals"},
        {"greaterOrEquals", "less"},
        {"greater", "lessOrEquals"},
        {"notIn", "in"},
        {"notLike", "like"},
        {"notEmpty", "empty"},
    };

    pushPullNotInAtom(atom, inverse_relations);
}

CNFQuery & CNFQuery::pullNotOutFunctions()
{
    transformAtoms([](const AtomicFormula & atom) -> AtomicFormula
                    {
                        AtomicFormula result{atom.negative, atom.ast->clone()};
                        pullNotOut(result);
                        return result;
                    });
    return *this;
}

CNFQuery & CNFQuery::pushNotInFuntions()
{
    transformAtoms([](const AtomicFormula & atom) -> AtomicFormula
                   {
                       AtomicFormula result{atom.negative, atom.ast->clone()};
                       pushNotIn(result);
                       return result;
                   });
    return *this;
}

namespace
{
    CNFQuery::AndGroup reduceOnce(const CNFQuery::AndGroup & groups)
    {
        CNFQuery::AndGroup result;
        for (const CNFQuery::OrGroup & group : groups)
        {
            CNFQuery::OrGroup copy(group);
            bool inserted = false;
            for (const CNFQuery::AtomicFormula & atom : group)
            {
                copy.erase(atom);
                CNFQuery::AtomicFormula negative_atom(atom);
                negative_atom.negative = !atom.negative;
                copy.insert(negative_atom);

                if (groups.contains(copy))
                {
                    copy.erase(negative_atom);
                    result.insert(copy);
                    inserted = true;
                    break;
                }

                copy.erase(negative_atom);
                copy.insert(atom);
            }
            if (!inserted)
                result.insert(group);
        }
        return result;
    }

    bool isSubset(const CNFQuery::OrGroup & left, const CNFQuery::OrGroup & right)
    {
        if (left.size() > right.size())
            return false;
        for (const auto & elem : left)
            if (!right.contains(elem))
                return false;
        return true;
    }

    CNFQuery::AndGroup filterSubsets(const CNFQuery::AndGroup & groups)
    {
        CNFQuery::AndGroup result;
        for (const CNFQuery::OrGroup & group : groups)
        {
            bool insert = true;

            for (const CNFQuery::OrGroup & other_group : groups)
            {
                if (isSubset(other_group, group) && group != other_group)
                {
                    insert = false;
                    break;
                }
            }

            if (insert)
                result.insert(group);
        }
        return result;
    }
}

CNFQuery & CNFQuery::reduce()
{
    while (true)
    {
        AndGroup new_statements = reduceOnce(statements);
        if (statements == new_statements)
        {
            statements = filterSubsets(statements);
            return *this;
        }
        else
            statements = new_statements;
    }
}

std::string CNFQuery::dump() const
{
    WriteBufferFromOwnString res;
    bool first = true;
    for (const auto & group : statements)
    {
        if (!first)
            res << " AND ";
        first = false;
        res << "(";
        bool first_in_group = true;
        for (const auto & atom : group)
        {
            if (!first_in_group)
                res << " OR ";
            first_in_group = false;
            if (atom.negative)
                res << " NOT ";
            res << atom.ast->getColumnName();
        }
        res << ")";
    }

    return res.str();
}

}
