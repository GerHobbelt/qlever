// Copyright 2014, University of Freiburg,
// Chair of Algorithms and Data Structures.
// Author: Björn Buchhold (buchhold@informatik.uni-freiburg.de)

#include "ParsedQuery.h"

#include <absl/strings/str_join.h>
#include <absl/strings/str_split.h>
#include <parser/RdfEscaping.h>
#include <util/Conversions.h>

#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using std::string;
using std::vector;

// _____________________________________________________________________________
string ParsedQuery::asString() const {
  std::ostringstream os;

  bool usesSelect = hasSelectClause();
  bool usesAsterisk = usesSelect && selectClause().isAsterisk();

  if (usesSelect) {
    const auto& selectClause = this->selectClause();
    // SELECT
    os << "\nSELECT: {\n\t";
    // TODO<joka921> is this needed?
    /*
    os <<
    absl::StrJoin(selectClause.varsAndAliasesOrAsterisk_.getSelectedVariables(),
                        ", ");
                        */
    os << "\n}";

    // ALIASES
    os << "\nALIASES: {\n\t";
    if (!usesAsterisk) {
      for (const auto& alias : selectClause.getAliases()) {
        os << alias._expression.getDescriptor() << "\n\t";
      }
      os << "{";
    }
  } else if (hasConstructClause()) {
    const auto& constructClause = this->constructClause().triples_;
    os << "\n CONSTRUCT {\n\t";
    for (const auto& triple : constructClause) {
      os << triple[0].toSparql();
      os << ' ';
      os << triple[1].toSparql();
      os << ' ';
      os << triple[2].toSparql();
      os << " .\n";
    }
    os << "}";
  }

  // WHERE
  os << "\nWHERE: \n";
  _rootGraphPattern.toString(os, 1);

  os << "\nLIMIT: " << (_limitOffset._limit);
  os << "\nTEXTLIMIT: " << (_limitOffset._textLimit);
  os << "\nOFFSET: " << (_limitOffset._offset);
  if (usesSelect) {
    const auto& selectClause = this->selectClause();
    os << "\nDISTINCT modifier is " << (selectClause.distinct_ ? "" : "not ")
       << "present.";
    os << "\nREDUCED modifier is " << (selectClause.reduced_ ? "" : "not ")
       << "present.";
  }
  os << "\nORDER BY: ";
  if (_orderBy.empty()) {
    os << "not specified";
  } else {
    for (auto& key : _orderBy) {
      os << key.variable_.name() << (key.isDescending_ ? " (DESC)" : " (ASC)")
         << "\t";
    }
  }
  os << "\n";
  return std::move(os).str();
}

// _____________________________________________________________________________
string SparqlPrefix::asString() const {
  std::ostringstream os;
  os << "{" << _prefix << ": " << _uri << "}";
  return std::move(os).str();
}

// _____________________________________________________________________________
string SparqlTriple::asString() const {
  std::ostringstream os;
  os << "{s: " << _s << ", p: " << _p << ", o: " << _o << "}";
  return std::move(os).str();
}

// ________________________________________________________________________
Variable ParsedQuery::addInternalBind(
    sparqlExpression::SparqlExpressionPimpl expression) {
  // Internal variable name to which the result of the helper bind is
  // assigned.
  auto targetVariable = Variable{INTERNAL_VARIABLE_PREFIX +
                                 std::to_string(numInternalVariables_)};
  numInternalVariables_++;
  // Don't register the targetVariable as visible because it is used
  // internally and should not be selected by SELECT * (this is the `bool`
  // argument to `addBind`).
  // TODO<qup42, joka921> Implement "internal" variables, that can't be
  //  selected at all and can never interfere with variables from the
  //  query.
  addBind(std::move(expression), targetVariable, false);
  return targetVariable;
}

// ________________________________________________________________________
void ParsedQuery::addBind(sparqlExpression::SparqlExpressionPimpl expression,
                          Variable targetVariable, bool targetIsVisible) {
  if (targetIsVisible) {
    registerVariableVisibleInQueryBody(targetVariable);
  }
  parsedQuery::Bind bind{std::move(expression), std::move(targetVariable)};
  _rootGraphPattern._graphPatterns.emplace_back(std::move(bind));
}

// ________________________________________________________________________
void ParsedQuery::addSolutionModifiers(SolutionModifiers modifiers) {
  auto checkVariableIsVisible = [this](const Variable& var,
                                       const std::string& locationDescription,
                                       const ad_utility::HashSet<Variable>&
                                           additionalVisibleVariables = {}) {
    if (!ad_utility::contains(getVisibleVariables(), var) &&
        !additionalVisibleVariables.contains(var)) {
      throw InvalidQueryException("Variable " + var.name() + " was used by " +
                                  locationDescription +
                                  ", but is not defined in the query body.");
    }
  };
  auto checkUsedVariablesAreVisible =
      [&checkVariableIsVisible](
          const sparqlExpression::SparqlExpressionPimpl& expression,
          const std::string& locationDescription,
          const ad_utility::HashSet<Variable>& additionalVisibleVariables =
              {}) {
        for (const auto* var : expression.containedVariables()) {
          checkVariableIsVisible(*var,
                                 locationDescription + " in expression \"" +
                                     expression.getDescriptor() + "\"",
                                 additionalVisibleVariables);
        }
      };

  // Process groupClause
  auto processVariable = [this,
                          &checkVariableIsVisible](const Variable& groupKey) {
    checkVariableIsVisible(groupKey, "GROUP BY");

    _groupByVariables.emplace_back(groupKey.name());
  };
  auto processExpression =
      [this, &checkUsedVariablesAreVisible](
          sparqlExpression::SparqlExpressionPimpl groupKey) {
        checkUsedVariablesAreVisible(groupKey, "GROUP BY");
        auto helperTarget = addInternalBind(std::move(groupKey));
        _groupByVariables.emplace_back(helperTarget.name());
      };
  auto processAlias = [this](Alias groupKey) {
    parsedQuery::Bind helperBind{std::move(groupKey._expression),
                                 groupKey._target};
    _rootGraphPattern._graphPatterns.emplace_back(std::move(helperBind));
    registerVariableVisibleInQueryBody(groupKey._target);
    _groupByVariables.emplace_back(groupKey._target);
  };

  for (auto& orderKey : modifiers.groupByVariables_) {
    std::visit(
        ad_utility::OverloadCallOperator{processVariable, processExpression,
                                         processAlias},
        std::move(orderKey));
  }

  // Process havingClause
  // TODO<joka921, qup42> as soon as FILTER and HAVING support proper
  //  expressions, also add similar sanity checks for the HAVING clause here.
  _havingClauses = std::move(modifiers.havingClauses_);

  const bool isExplicitGroupBy = !_groupByVariables.empty();
  const bool isImplicitGroupBy =
      std::ranges::any_of(getAliases(),
                          [](const Alias& alias) {
                            return alias._expression.containsAggregate();
                          }) &&
      !isExplicitGroupBy;
  const bool isGroupBy = isExplicitGroupBy || isImplicitGroupBy;
  using namespace std::string_literals;
  std::string noteForImplicitGroupBy =
      isImplicitGroupBy
          ? " Note: The GROUP BY in this query is implicit because an aggregate expression was used in the SELECT clause"s
          : ""s;
  std::string noteForGroupByError =
      " All non-aggregated variables must be part of the GROUP BY "
      "clause." +
      noteForImplicitGroupBy;

  // Process orderClause
  auto processVariableOrderKey = [this, &checkVariableIsVisible, isGroupBy,
                                  &noteForImplicitGroupBy](
                                     VariableOrderKey orderKey) {
    // Check whether grouping is done. The variable being ordered by
    // must then be either grouped or the result of an alias in the select.
    const vector<Variable>& groupByVariables = _groupByVariables;

    if (!isGroupBy) {
      checkVariableIsVisible(orderKey.variable_, "ORDER BY");
    } else if (!ad_utility::contains(groupByVariables, orderKey.variable_) &&
               // `ConstructClause` has no Aliases. So the variable can never be
               // the result of an Alias.
               (hasConstructClause() ||
                !ad_utility::contains_if(selectClause().getAliases(),
                                         [&orderKey](const Alias& alias) {
                                           return alias._target ==
                                                  orderKey.variable_;
                                         }))) {
      throw InvalidQueryException(
          "Variable " + orderKey.variable_.name() +
          " was used in an ORDER BY "
          "clause, but is neither grouped, nor created as an alias in the "
          "SELECT clause." +
          noteForImplicitGroupBy);
    }

    _orderBy.push_back(std::move(orderKey));
  };

  // QLever currently only supports ordering by variables. To allow
  // all `orderConditions`, the corresponding expression is bound to a new
  // internal variable. Ordering is then done by this variable.
  auto processExpressionOrderKey = [this, &checkUsedVariablesAreVisible,
                                    isGroupBy, &noteForImplicitGroupBy](
                                       ExpressionOrderKey orderKey) {
    checkUsedVariablesAreVisible(orderKey.expression_, "ORDER BY");
    if (isGroupBy) {
      // TODO<qup42> Implement this by adding a hidden alias in the
      //  SELECT clause.
      throw NotSupportedException(
          "Ordering by an expression while grouping is not supported by "
          "QLever. (The expression is \"" +
          orderKey.expression_.getDescriptor() +
          "\"). Please assign this expression to a "
          "new variable in the SELECT clause and then order by this "
          "variable." +
          noteForImplicitGroupBy);
    }
    auto additionalVariable = addInternalBind(std::move(orderKey.expression_));
    _orderBy.emplace_back(additionalVariable, orderKey.isDescending_);
  };

  for (auto& orderKey : modifiers.orderBy_.orderKeys) {
    std::visit(ad_utility::OverloadCallOperator{processVariableOrderKey,
                                                processExpressionOrderKey},
               std::move(orderKey));
  }
  _isInternalSort = modifiers.orderBy_.isInternalSort;

  // Process limitOffsetClause
  _limitOffset = modifiers.limitOffset_;

  auto checkAliasTargetsHaveNoOverlap = [this]() {
    ad_utility::HashMap<Variable, size_t> variable_counts;

    for (const Variable& v : selectClause().getSelectedVariables()) {
      variable_counts[v]++;
    }

    for (const auto& alias : selectClause().getAliases()) {
      if (ad_utility::contains(selectClause().getVisibleVariables(),
                               alias._target)) {
        throw InvalidQueryException(absl::StrCat(
            "The target", alias._target.name(),
            " of an AS clause was already used in the query body."));
      }

      // The variable was already added to the selected variables while
      // parsing the alias, thus it should appear exactly once
      if (variable_counts[alias._target] > 1) {
        throw InvalidQueryException(absl::StrCat(
            "The target", alias._target.name(),
            " of an AS clause was already used before in the SELECT clause."));
      }
    }
  };

  if (hasSelectClause()) {
    checkAliasTargetsHaveNoOverlap();

    // Check that all the variables that are used in aliases are either visible
    // in the query body or are bound by a previous alias from the same SELECT
    // clause.
    // Note: Currently the reusage of variables from previous aliases
    // like SELECT (?a AS ?b) (?b AS ?c) is only supported by QLever if there is
    // no GROUP BY in the query. To support this we would also need changes in
    // the `GroupBy` class.
    // TODO<joka921> Implement these changes and support this case.
    ad_utility::HashSet<Variable> variablesBoundInAliases;
    for (const auto& alias : selectClause().getAliases()) {
      if (!isGroupBy) {
        checkUsedVariablesAreVisible(alias._expression, "SELECT",
                                     variablesBoundInAliases);
      } else {
        try {
          checkUsedVariablesAreVisible(alias._expression, "SELECT", {});
        } catch (const InvalidQueryException& ex) {
          // If the variable is neither defined in the query body nor in the
          // select clause before, then the following call will throw the same
          // exception that we have just caught. Else we are in the unsupported
          // case and throw a more useful error message.
          checkUsedVariablesAreVisible(alias._expression, "SELECT",
                                       variablesBoundInAliases);
          std::string_view note =
              " Note: This variable was defined previously in the SELECT "
              "clause, which is supported by the SPARQL standard, but "
              "currently not supported by QLever when the query contains a "
              "GROUP BY clause.";
          throw NotSupportedException{
              absl::StrCat(ex.errorMessageWithoutPrefix(), note,
                           noteForGroupByError),
              ex.metadata()};
        }
      }
      variablesBoundInAliases.insert(alias._target);
    }

    if (isGroupBy) {
      ad_utility::HashSet<string> groupVariables{};
      for (const auto& variable : _groupByVariables) {
        groupVariables.emplace(variable.toSparql());
      }

      if (selectClause().isAsterisk()) {
        throw InvalidQueryException(
            "GROUP BY is not allowed when all variables are selected via "
            "SELECT *");
      }

      // Check if all selected variables are either aggregated or
      // part of the group by statement.
      const auto& aliases = selectClause().getAliases();
      for (const Variable& var : selectClause().getSelectedVariables()) {
        if (auto it = std::ranges::find(aliases, var, &Alias::_target);
            it != aliases.end()) {
          const auto& alias = *it;
          if (alias._expression.isAggregate(groupVariables)) {
            continue;
          } else {
            auto unaggregatedVars =
                alias._expression.getUnaggregatedVariables(groupVariables);
            throw InvalidQueryException(absl::StrCat(
                "The expression \"", alias._expression.getDescriptor(),
                "\" does not aggregate ", absl::StrJoin(unaggregatedVars, ", "),
                "." + noteForGroupByError));
          }
        }
        if (!ad_utility::contains(_groupByVariables, var)) {
          throw InvalidQueryException(absl::StrCat(
              "Variable ", var.name(), " is selected but not aggregated.",
              noteForGroupByError));
        }
      }
    } else {
      // If there is no GROUP BY clause and there is a SELECT clause, then the
      // aliases like SELECT (?x as ?y) have to be added as ordinary BIND
      // expressions to the query body. In CONSTRUCT queries there are no such
      // aliases, and in case of a GROUP BY clause the aliases are read
      // directly from the SELECT clause by the `GroupBy` operation.

      auto& selectClause = std::get<SelectClause>(_clause);
      for (const auto& alias : selectClause.getAliases()) {
        // As the clause is NOT `SELECT *` it is not required to register the
        // target variable as visible, but it helps with several sanity
        // checks.
        addBind(alias._expression, alias._target, true);
      }

      // We do not need the aliases anymore as we have converted them to BIND
      // expressions
      selectClause.deleteAliasesButKeepVariables();
    }
  } else {
    AD_CORRECTNESS_CHECK(hasConstructClause());
    if (_groupByVariables.empty()) {
      return;
    }

    for (const auto& variable : constructClause().containedVariables()) {
      if (!ad_utility::contains(_groupByVariables, variable)) {
        throw InvalidQueryException("Variable " + variable.name() +
                                    " is used but not aggregated." +
                                    noteForGroupByError);
      }
    }
  }
}

void ParsedQuery::merge(const ParsedQuery& p) {
  auto& children = _rootGraphPattern._graphPatterns;
  auto& otherChildren = p._rootGraphPattern._graphPatterns;
  children.insert(children.end(), otherChildren.begin(), otherChildren.end());

  // update the ids
  _numGraphPatterns = 0;
  _rootGraphPattern.recomputeIds(&_numGraphPatterns);
}

// _____________________________________________________________________________
const std::vector<Variable>& ParsedQuery::getVisibleVariables() const {
  return std::visit(&parsedQuery::ClauseBase::getVisibleVariables, _clause);
}

// _____________________________________________________________________________
void ParsedQuery::registerVariablesVisibleInQueryBody(
    const vector<Variable>& variables) {
  for (const auto& var : variables) {
    registerVariableVisibleInQueryBody(var);
  }
}

// _____________________________________________________________________________
void ParsedQuery::registerVariableVisibleInQueryBody(const Variable& variable) {
  auto addVariable = [&variable](auto& clause) {
    clause.addVisibleVariable(variable);
  };
  std::visit(addVariable, _clause);
}

// _____________________________________________________________________________
void ParsedQuery::GraphPattern::toString(std::ostringstream& os,
                                         int indentation) const {
  for (int j = 1; j < indentation; ++j) os << "  ";
  os << "{";
  for (size_t i = 0; i + 1 < _filters.size(); ++i) {
    os << "\n";
    for (int j = 0; j < indentation; ++j) os << "  ";
    os << _filters[i].asString() << ',';
  }
  if (_filters.size() > 0) {
    os << "\n";
    for (int j = 0; j < indentation; ++j) os << "  ";
    os << _filters.back().asString();
  }
  for (const GraphPatternOperation& child : _graphPatterns) {
    os << "\n";
    child.toString(os, indentation + 1);
  }
  os << "\n";
  for (int j = 1; j < indentation; ++j) os << "  ";
  os << "}";
}

// _____________________________________________________________________________
void ParsedQuery::GraphPattern::recomputeIds(size_t* id_count) {
  bool allocatedIdCounter = false;
  if (id_count == nullptr) {
    id_count = new size_t(0);
    allocatedIdCounter = true;
  }
  _id = *id_count;
  (*id_count)++;
  for (auto& op : _graphPatterns) {
    op.visit([&id_count](auto&& arg) {
      using T = std::decay_t<decltype(arg)>;
      if constexpr (std::is_same_v<T, parsedQuery::Union>) {
        arg._child1.recomputeIds(id_count);
        arg._child2.recomputeIds(id_count);
      } else if constexpr (std::is_same_v<T, parsedQuery::Optional> ||
                           std::is_same_v<T, parsedQuery::GroupGraphPattern> ||
                           std::is_same_v<T, parsedQuery::Minus>) {
        arg._child.recomputeIds(id_count);
      } else if constexpr (std::is_same_v<T, parsedQuery::TransPath>) {
        // arg._childGraphPattern.recomputeIds(id_count);
      } else if constexpr (std::is_same_v<T, parsedQuery::Values>) {
        arg._id = (*id_count)++;
      } else {
        static_assert(std::is_same_v<T, parsedQuery::Subquery> ||
                      std::is_same_v<T, parsedQuery::Service> ||
                      std::is_same_v<T, parsedQuery::BasicGraphPattern> ||
                      std::is_same_v<T, parsedQuery::Bind>);
        // subquery children have their own id space
        // TODO:joka921 look at the optimizer if it is ok, that
        // BasicGraphPatterns and Values have no ids at all. at the same time
        // assert that the above else-if is exhaustive.
      }
    });
  }

  if (allocatedIdCounter) {
    delete id_count;
  }
}

// __________________________________________________________________________
ParsedQuery::GraphPattern::GraphPattern() : _optional(false) {}

// TODO<joka921> Change the first argument to `Variable`, but first merge
// the filter-PR.
void ParsedQuery::GraphPattern::addLanguageFilter(
    const Variable& variable, const std::string& languageInQuotes) {
  auto langTag = languageInQuotes.substr(1, languageInQuotes.size() - 2);
  // Find all triples where the object is the `variable` and the predicate is
  // a simple `IRIREF` (neither a variable nor a complex property path).
  // Search in all the basic graph patterns, as filters have the complete
  // graph patterns as their scope.
  // TODO<joka921> In theory we could also recurse into GroupGraphPatterns,
  // Subqueries etc.
  // TODO<joka921> Also support property paths (^rdfs:label,
  // skos:altLabel|rdfs:label, ...)
  std::vector<SparqlTriple*> matchingTriples;
  for (auto& graphPattern : _graphPatterns) {
    auto* basicPattern =
        std::get_if<parsedQuery::BasicGraphPattern>(&graphPattern);
    if (!basicPattern) {
      continue;
    }
    for (auto& triple : basicPattern->_triples) {
      if (triple._o == variable &&
          (triple._p._operation == PropertyPath::Operation::IRI &&
           !isVariable(triple._p))) {
        matchingTriples.push_back(&triple);
      }
    }
  }

  // Replace all the matching triples.
  for (auto* triplePtr : matchingTriples) {
    triplePtr->_p._iri = '@' + langTag + '@' + triplePtr->_p._iri;
  }

  // Handle the case, that no suitable triple (see above) was found. In this
  // case a triple `?variable ql:langtag "language"` is added at the end of
  // the graph pattern.
  if (matchingTriples.empty()) {
    LOG(DEBUG) << "language filter variable " + variable.name() +
                      " did not appear as object in any suitable "
                      "triple. "
                      "Using literal-to-language predicate instead.\n";

    // If necessary create an empty `BasicGraphPattern` at the end to which we
    // can append a triple.
    // TODO<joka921> It might be beneficial to place this triple not at the
    // end but close to other occurences of `variable`.
    if (_graphPatterns.empty() ||
        !std::holds_alternative<parsedQuery::BasicGraphPattern>(
            _graphPatterns.back())) {
      _graphPatterns.emplace_back(parsedQuery::BasicGraphPattern{});
    }
    auto& t = std::get<parsedQuery::BasicGraphPattern>(_graphPatterns.back())
                  ._triples;

    auto langEntity = ad_utility::convertLangtagToEntityUri(langTag);
    SparqlTriple triple(variable, PropertyPath::fromIri(LANGUAGE_PREDICATE),
                        langEntity);
    t.push_back(std::move(triple));
  }
}

// ____________________________________________________________________________
const std::vector<Alias>& ParsedQuery::getAliases() const {
  if (hasSelectClause()) {
    return selectClause().getAliases();
  } else {
    static const std::vector<Alias> dummyForConstructClause;
    return dummyForConstructClause;
  }
}

// ____________________________________________________________________________
cppcoro::generator<const Variable>
ParsedQuery::getConstructedOrSelectedVariables() const {
  if (hasSelectClause()) {
    for (const auto& variable : selectClause().getSelectedVariables()) {
      co_yield variable;
    }
  } else {
    for (const auto& variable : constructClause().containedVariables()) {
      co_yield variable;
    }
  }
  // Nothing to yield in the CONSTRUCT case.
}
