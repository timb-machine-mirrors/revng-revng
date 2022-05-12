#pragma once

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

#include "InternalGraph.h"
#include "NodeClassification.h"

/// Prepares the graph for futher processing.
template<RankingStrategy Strategy>
std::tuple<InternalGraph, RankContainer, NodeClassifier<Strategy>>
prepareGraph(ExternalGraph &Graph);

/// Computes the layout given a graph and the configuration.
///
/// \note: it only works with `MutableEdgeNode`s.
template<yield::sugiyama::RankingStrategy RS>
inline bool calculateSugiyamaLayout(ExternalGraph &Graph,
                                    const Configuration &Configuration) {
  static_assert(IsMutableEdgeNode<InternalNode>,
                "LayouterSugiyama requires mutable edge nodes.");

  // Prepare the graph for the layouter: this converts `Graph` into
  // an internal graph and guaranties that it's has no loops (some of the
  // edges might have to be temporarily inverted to ensure this), a single
  // entry point (an extra node might have to be added) and that both
  // long edges and backwards facing edges are split up into into chunks
  // that span at most one layer at a time.
  auto [DAG, Ranks, Classified] = prepareGraph<RS>(Graph);

  return true;
}
