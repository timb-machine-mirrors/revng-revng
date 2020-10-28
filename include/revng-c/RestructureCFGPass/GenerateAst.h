#ifndef REVNGC_RESTRUCTURE_CFG_GENERATEAST_H
#define REVNGC_RESTRUCTURE_CFG_GENERATEAST_H

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Standard includes
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sys/stat.h>

// LLVM includes
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/GenericDomTreeConstruction.h>
#include <llvm/Support/raw_os_ostream.h>

// revng includes
#include <revng/BasicAnalyses/GeneratedCodeBasicInfo.h>
#include <revng/Support/IRHelpers.h>

// Local libraries includes
#include "revng-c/ADT/ReversePostOrderTraversal.h"
#include "revng-c/RestructureCFGPass/ASTTree.h"
#include "revng-c/RestructureCFGPass/BasicBlockNodeBB.h"
#include "revng-c/RestructureCFGPass/MetaRegionBB.h"
#include "revng-c/RestructureCFGPass/RegionCFGTree.h"
#include "revng-c/RestructureCFGPass/Utils.h"

// Helper function that visit an AST tree and creates the sequence nodes
inline ASTNode *createSequence(ASTTree &Tree, ASTNode *RootNode) {
  SequenceNode *RootSequenceNode = Tree.addSequenceNode();
  RootSequenceNode->addNode(RootNode);

  for (ASTNode *Node : RootSequenceNode->nodes()) {

    switch (Node->getKind()) {

    case ASTNode::NK_If: {
      auto *If = llvm::cast<IfNode>(Node);

      if (If->hasThen())
        If->setThen(createSequence(Tree, If->getThen()));
      if (If->hasElse())
        If->setElse(createSequence(Tree, If->getElse()));
    } break;

    case ASTNode::NK_Switch: {
      auto *Switch = llvm::cast<SwitchNode>(Node);
      for (auto &LabelCasePair : Switch->cases())
        LabelCasePair.second = createSequence(Tree, LabelCasePair.second);

      if (ASTNode *Default = Switch->getDefault())
        Switch->replaceDefault(createSequence(Tree, Default));
    } break;

    case ASTNode::NK_Scs: {
      auto *Scs = llvm::cast<ScsNode>(Node);
      if (Scs->hasBody())
        Scs->setBody(createSequence(Tree, Scs->getBody()));
    } break;

    case ASTNode::NK_Code: {
      // TODO: confirm that doesn't make sense to process a code node.
    } break;

    case ASTNode::NK_Continue:
    case ASTNode::NK_Break:
    case ASTNode::NK_SwitchBreak:
    case ASTNode::NK_Set: {
      // Do nothing for these nodes
    } break;

    case ASTNode::NK_List:
    default:
      revng_abort("AST node type not expected");
    }
  }

  return RootSequenceNode;
}

// Helper function that simplifies useless dummy nodes
inline void simplifyDummies(ASTNode *RootNode) {

  switch (RootNode->getKind()) {

  case ASTNode::NK_List: {
    auto *Sequence = llvm::cast<SequenceNode>(RootNode);
    std::vector<ASTNode *> UselessDummies;
    for (ASTNode *Node : Sequence->nodes()) {
      if (Node->isEmpty()) {
        UselessDummies.push_back(Node);
      } else {
        simplifyDummies(Node);
      }
    }
    for (ASTNode *Node : UselessDummies) {
      Sequence->removeNode(Node);
    }
  } break;

  case ASTNode::NK_If: {
    auto *If = llvm::cast<IfNode>(RootNode);
    if (If->hasThen()) {
      simplifyDummies(If->getThen());
    }
    if (If->hasElse()) {
      simplifyDummies(If->getElse());
    }
  } break;

  case ASTNode::NK_Switch: {

    auto *Switch = llvm::cast<SwitchNode>(RootNode);

    for (auto &LabelCaseNodePair : Switch->cases())
      simplifyDummies(LabelCaseNodePair.second);

    if (auto *Default = Switch->getDefault())
      simplifyDummies(Default);

  } break;

  case ASTNode::NK_Scs: {
    auto *Scs = llvm::cast<ScsNode>(RootNode);
    if (Scs->hasBody())
      simplifyDummies(Scs->getBody());
  } break;

  case ASTNode::NK_Code:
  case ASTNode::NK_Continue:
  case ASTNode::NK_Break:
  case ASTNode::NK_SwitchBreak:
  case ASTNode::NK_Set:
    // Do nothing
    break;

  default:
    revng_unreachable();
  }
}

// Helper function which simplifies sequence nodes composed by a single AST
// node.
inline ASTNode *simplifyAtomicSequence(ASTTree &AST, ASTNode *RootNode) {
  switch (RootNode->getKind()) {

  case ASTNode::NK_List: {
    auto *Sequence = llvm::cast<SequenceNode>(RootNode);
    switch (Sequence->listSize()) {

    case 0:
      RootNode = nullptr;
      break;

    case 1:
      RootNode = simplifyAtomicSequence(AST, Sequence->getNodeN(0));
      break;

    default:
      bool Empty = true;
      for (ASTNode *&Node : Sequence->nodes()) {
        Node = simplifyAtomicSequence(AST, Node);
        if (nullptr != Node)
          Empty = false;
      }
      revng_assert(not Empty);
    }
  } break;

  case ASTNode::NK_If: {
    auto *If = llvm::cast<IfNode>(RootNode);

    if (If->hasThen())
      If->setThen(simplifyAtomicSequence(AST, If->getThen()));

    if (If->hasElse())
      If->setElse(simplifyAtomicSequence(AST, If->getElse()));

  } break;

  case ASTNode::NK_Switch: {

    auto *Switch = llvm::cast<SwitchNode>(RootNode);

    // In case the recursive call to `simplifyAtomicSequence` gives origin to a
    // complete simplification of the default node of the switch, setting its
    // corresponding `ASTNode` to `nullptr` already does the job, since having
    // the corresponding `Default` field set to `nullptr` means that the switch
    // node has no default.
    if (ASTNode *Default = Switch->getDefault()) {
      auto *NewDefault = simplifyAtomicSequence(AST, Default);
      if (NewDefault != Default)
        Switch->replaceDefault(NewDefault);
    }

    auto LabelCasePairIt = Switch->cases().begin();
    auto LabelCasePairEnd = Switch->cases().end();
    while (LabelCasePairIt != LabelCasePairEnd) {
      auto *NewCaseNode = simplifyAtomicSequence(AST, LabelCasePairIt->second);
      if (nullptr == NewCaseNode) {
        if (nullptr == Switch->getDefault()) {
          LabelCasePairIt = Switch->cases().erase(LabelCasePairIt);
          LabelCasePairEnd = Switch->cases().end();
        } else {
          LabelCasePairIt->second = AST.addSwitchBreak();
        }
      } else {
        LabelCasePairIt->second = NewCaseNode;
        ++LabelCasePairIt;
      }
    }

  } break;

  case ASTNode::NK_Scs: {
    auto *Scs = llvm::cast<ScsNode>(RootNode);
    if (Scs->hasBody())
      Scs->setBody(simplifyAtomicSequence(AST, Scs->getBody()));
  } break;

  case ASTNode::NK_Code:
  case ASTNode::NK_Continue:
  case ASTNode::NK_Break:
  case ASTNode::NK_SwitchBreak:
  case ASTNode::NK_Set:
    // Do nothing
    break;

  default:
    revng_unreachable();
  }

  return RootNode;
}

template<class NodeT>
inline BasicBlockNode<NodeT> *getDirectSuccessor(BasicBlockNode<NodeT> *Node) {
  BasicBlockNode<NodeT> *Successor = nullptr;
  if (Node->successor_size() == 1) {
    Successor = Node->getSuccessorI(0);
  }
  return Successor;
}

template<class NodeT>
inline BasicBlockNode<NodeT> *
findCommonPostDom(BasicBlockNode<NodeT> *Succ1, BasicBlockNode<NodeT> *Succ2) {

  // Retrieve the successor of the two successors of the `IfNode`, and check
  // that or the retrieved node is equal for both the successors, or it does
  // not exists for both of them.
  BasicBlockNode<NodeT> *SuccOfSucc1 = nullptr;
  BasicBlockNode<NodeT> *SuccOfSucc2 = nullptr;

  revng_assert(Succ1->successor_size() < 2);
  revng_assert(Succ2->successor_size() < 2);

  if (Succ1->successor_size() == 1)
    SuccOfSucc1 = getDirectSuccessor(Succ1);

  if (Succ2->successor_size() == 1)
    SuccOfSucc2 = getDirectSuccessor(Succ2);

  revng_assert(SuccOfSucc1 == SuccOfSucc2);

  return SuccOfSucc1;
}

template<class NodeT>
inline ASTNode *findASTNode(ASTTree &AST,
                            typename RegionCFG<NodeT>::BBNodeMap &TileToNodeMap,
                            BasicBlockNode<NodeT> *Node) {
  if (TileToNodeMap.count(Node) != 0) {
    Node = TileToNodeMap.at(Node);
  }

  return AST.findASTNode(Node);
}

template<class NodeT>
inline void
createTile(RegionCFG<NodeT> &Graph,
           llvm::DominatorTreeBase<BasicBlockNode<NodeT>, false> &ASTDT,
           typename RegionCFG<NodeT>::BBNodeMap &TileToNodeMap,
           BasicBlockNode<NodeT> *Node,
           BasicBlockNode<NodeT> *End) {

  using BBNodeT = BasicBlockNode<NodeT>;
  using BasicBlockNodeTVect = std::vector<BBNodeT *>;
  using EdgeDescriptor = typename BBNodeT::EdgeDescriptor;

  // Create the new tile node.
  BBNodeT *Tile = Graph.addTile();

  // Move all the edges incoming in the head of the collapsed region to the tile
  // node.
  BasicBlockNodeTVect Predecessors;
  for (BBNodeT *Predecessor : Node->predecessors())
    Predecessors.push_back(Predecessor);

  for (BBNodeT *Predecessor : Predecessors) {
    auto Edge = extractLabeledEdge(EdgeDescriptor{ Predecessor, Node });
    ASTDT.deleteEdge(Predecessor, Node);
    addEdge(EdgeDescriptor{ Predecessor, Tile }, Edge.second);
    ASTDT.insertEdge(Predecessor, Tile);
  }

  // Move all the edges exiting from the postdominator node of the collapsed
  // region to the tile node, if the `End` passed as an argument is
  // `!= nullptr`.
  if (End != nullptr) {

    BasicBlockNodeTVect Successors;
    for (BBNodeT *Successor : End->successors())
      Successors.push_back(Successor);

    for (BBNodeT *Successor : Successors) {
      auto Edge = extractLabeledEdge(EdgeDescriptor{ End, Successor });
      ASTDT.deleteEdge(End, Successor);
      addEdge(EdgeDescriptor{ Tile, Successor }, Edge.second);
      ASTDT.insertEdge(Tile, Successor);
    }
  }

  // Update the map containing the mapping between tiles and the head node which
  // gave origin to a certain tile.
  TileToNodeMap[Tile] = Node;
}

template<class NodeT>
inline void generateAst(RegionCFG<NodeT> &Region,
                        ASTTree &AST,
                        typename RegionCFG<NodeT>::DuplicationMap &NDuplicates,
                        std::map<RegionCFG<NodeT> *, ASTTree> &CollapsedMap) {
  // Define some using used in all the function body.
  using BasicBlockNodeT = typename RegionCFG<NodeT>::BasicBlockNodeT;
  using BasicBlockNodeTVect = typename RegionCFG<NodeT>::BasicBlockNodeTVect;

  // Get some fields of `RegionCFG`.
  std::string RegionName = Region.getRegionName();
  std::string FunctionName = Region.getFunctionName();

  RegionCFG<NodeT> &Graph = Region;

  Graph.markUnexpectedAndAnyPCAsInlined();

  if (CombLogger.isEnabled()) {
    CombLogger << "Weaveing region " + RegionName + "\n";
    Graph.dumpDotOnFile("weaves", FunctionName, "PREWEAVE");
  }

  // Invoke the weave function.
  Graph.weave();

  if (CombLogger.isEnabled()) {
    Graph.dumpDotOnFile("weaves", FunctionName, "POSTWEAVE");

    CombLogger << "Inflating region " + RegionName + "\n";
    Graph.dumpDotOnFile("dots", FunctionName, "PRECOMB");
  }

  Graph.inflate();
  if (CombLogger.isEnabled()) {
    Graph.dumpDotOnFile("dots", FunctionName, "POSTCOMB");
  }

  // Compute the NDuplicates, which will be used later.
  // TODO: this now doesn't run multiple
  for (BasicBlockNodeBB *BBNode : Graph.nodes()) {
    if (BBNode->isCode()) {
      llvm::BasicBlock *BB = BBNode->getOriginalNode();
      NDuplicates[BB] += 1;
    }
  }

  // TODO: factorize out the AST generation phase.
  llvm::DominatorTreeBase<BasicBlockNode<NodeT>, false> ASTDT;
  ASTDT.recalculate(Graph);

  CombLogger << DoLog;

  std::map<BasicBlockNode<NodeT> *, BasicBlockNode<NodeT> *> TileToNodeMap;

  BasicBlockNodeTVect PONodes;
  for (auto *N : post_order(&Graph))
    PONodes.push_back(N);

  unsigned Counter = 0;
  for (BasicBlockNode<NodeT> *Node : PONodes) {

    if (CombLogger.isEnabled()) {
      Counter++;
      Graph.dumpDotOnFile("dots",
                          FunctionName,
                          "AST-" + std::to_string(Counter));
    }

    // Collect the children nodes in the dominator tree.
    llvm::SmallVector<decltype(Node), 8> Children;
    {
      const auto &DomTreeChildren = ASTDT[Node]->getChildren();
      for (auto *DomTreeNode : DomTreeChildren)
        Children.push_back(DomTreeNode->getBlock());
    }

    // Collect the successor nodes of the current analyzed node.
    llvm::SmallVector<decltype(Node), 8> Successors;
    for (BasicBlockNode<NodeT> *Successor : Node->successors())
      Successors.push_back(Successor);

    // Handle collapsded node.
    ASTTree::ast_unique_ptr ASTObject;
    if (Node->isCollapsed()) {

      revng_assert(Children.size() <= 1);
      RegionCFG<NodeT> *BodyGraph = Node->getCollapsedCFG();
      revng_assert(BodyGraph != nullptr);
      revng_log(CombLogger,
                "Inspecting collapsed node: " << Node->getNameStr());

      // Call recursively the generation of the AST for the collapsed node.
      const auto &[It, New] = CollapsedMap.insert({ BodyGraph, ASTTree() });
      ASTTree &CollapsedAST = It->second;
      if (New)
        generateAst(*BodyGraph, CollapsedAST, NDuplicates, CollapsedMap);
      ASTNode *Body = AST.copyASTNodesFrom(CollapsedAST);

      switch (Successors.size()) {

      case 0: {
        ASTObject.reset(new ScsNode(Node, Body));
      } break;

      case 1: {
        auto *Succ = Successors[0];
        ASTNode *ASTChild = nullptr;
        if (ASTDT.dominates(Node, Succ)) {
          ASTChild = findASTNode(AST, TileToNodeMap, Succ);
          createTile(Graph, ASTDT, TileToNodeMap, Node, Succ);
        }
        ASTObject.reset(new ScsNode(Node, Body, ASTChild));
      } break;

      default:
        revng_abort();
      }

    } else if (Node->isDispatcher() or isASwitch(Node)) {

      revng_assert(Node->isCode() or Node->isDispatcher());

      llvm::Value *SwitchCondition = nullptr;
      if (not Node->isDispatcher()) {
        llvm::BasicBlock *OriginalNode = Node->getOriginalNode();
        llvm::Instruction *Terminator = OriginalNode->getTerminator();
        llvm::SwitchInst *Switch = llvm::cast<llvm::SwitchInst>(Terminator);
        SwitchCondition = Switch->getCondition();
      }
      revng_assert(SwitchCondition or Node->isDispatcher());

      auto NumSucc = Node->successor_size();
      revng_assert(NumSucc);
      BasicBlockNodeT *Fallthrough = nullptr;
      for (const auto &[SwitchSucc, EdgeInfos] : Node->labeled_successors()) {
        if (ASTDT.dominates(Node, SwitchSucc) or EdgeInfos.Inlined)
          continue;

        revng_assert(not Fallthrough);
        Fallthrough = SwitchSucc;
      }

      SwitchNode::case_container LabeledCases;
      ASTNode *DefaultASTNode = nullptr;
      for (const auto &[SwitchSucc, EdgeInfos] : Node->labeled_successors()) {

        ASTNode *ASTPointer = nullptr;
        if (SwitchSucc == Fallthrough)
          ASTPointer = AST.addSwitchBreak();
        else
          ASTPointer = findASTNode(AST, TileToNodeMap, SwitchSucc);

        revng_assert(nullptr != ASTPointer);

        if (EdgeInfos.Labels.empty()) {
          revng_assert(nullptr == DefaultASTNode);
          DefaultASTNode = ASTPointer;
        }

        LabeledCases.push_back({ EdgeInfos.Labels, ASTPointer });
      }

      revng_assert(DefaultASTNode or Node->isWeaved() or Node->isDispatcher());
      revng_assert(Node->successor_size() == LabeledCases.size());

      revng_assert(not Fallthrough or Children.size() < Node->successor_size());
      revng_assert(Fallthrough or Children.size() >= Node->successor_size());

      ASTNode *PostDomASTNode = nullptr;
      BasicBlockNodeT *PostDomBB = nullptr;
      // If we have the fallthrough we should not look for the post-dominator of
      // the switch, because the post-dominator is now the fallthrough.
      // If we don't have the fallthrough we might have a post-dominator for the
      // switch and need to find it to generate the correct ast.
      if (not Fallthrough) {
        if (Children.size() > Node->successor_size()) {
          // There are some children on the dominator tree that are not
          // successors on the graph. It should be at most one, which is the
          // post-dominator.
          const auto NotSuccessor = [&Node](const auto *Child) {
            auto It = Node->successors().begin();
            auto End = Node->successors().end();
            return std::find(It, End, Child) == End;
          };

          auto It = std::find_if(Children.begin(),
                                 Children.end(),
                                 NotSuccessor);

          // Assert that we found one.
          revng_assert(It != Children.end());

          PostDomASTNode = findASTNode(AST, TileToNodeMap, *It);
          PostDomBB = *It;

          // Assert that we don't find more than one.
          It = std::find_if(std::next(It), Children.end(), NotSuccessor);
          revng_assert(It == Children.end());
        }
      }

      createTile(Graph, ASTDT, TileToNodeMap, Node, PostDomBB);

      ASTObject.reset(new SwitchNode(Node,
                                     SwitchCondition,
                                     std::move(LabeledCases),
                                     DefaultASTNode,
                                     PostDomASTNode));
    } else {
      switch (Successors.size()) {

      case 2: {

        switch (Children.size()) {

        case 0: {

          // This means that both our exiting edges have been inlined, and we
          // do not have any immediate postdominator. This situation should
          // not arise, since not having at least one of the two branches
          // dominated is a signal of an error.
          revng_assert(not Node->isBreak() and not Node->isContinue()
                       and not Node->isSet());
          revng_log(CombLogger,
                    "Node " << Node->getNameStr()
                            << " does not dominate any "
                               "node, but has two successors.");
          revng_unreachable("A node does not dominate any node, but has two "
                            "successors.");
        } break;
        case 1: {

          // This means that we have two successors, but we only dominate a
          // single node. This situation is possible only if we have an
          // inlined edge and we have nopostdominator in the tile.
          revng_assert(not Node->isBreak() and not Node->isContinue()
                       and not Node->isSet());
          BasicBlockNode<NodeT> *Successor1 = Successors[0];
          BasicBlockNode<NodeT> *Successor2 = Successors[1];

          ASTNode *Then = nullptr;
          ASTNode *Else = nullptr;
          BasicBlockNodeT *NotDominatedSucc = nullptr;

          using ConstEdge = std::pair<const BasicBlockNodeT *,
                                      const BasicBlockNodeT *>;

          bool Inlined1 = isEdgeInlined(ConstEdge{ Node, Successor1 });
          bool Inlined2 = isEdgeInlined(ConstEdge{ Node, Successor2 });

          if (Inlined1 and Inlined2) {
            revng_assert(ASTDT.dominates(Node, Successor1));
            revng_assert(ASTDT.dominates(Node, Successor2));
            Then = findASTNode(AST, TileToNodeMap, Successor1);
            Else = findASTNode(AST, TileToNodeMap, Successor2);
          } else if (Inlined1) {
            revng_assert(ASTDT.dominates(Node, Successor1));
            Then = findASTNode(AST, TileToNodeMap, Successor1);
            NotDominatedSucc = Successor2;
          } else if (Inlined2) {
            revng_assert(ASTDT.dominates(Node, Successor2));
            Else = findASTNode(AST, TileToNodeMap, Successor2);
            NotDominatedSucc = Successor1;
          } else {
            auto *DominatedSucc = Children[0];
            revng_assert(DominatedSucc == Successor1
                         or DominatedSucc == Successor2);
            NotDominatedSucc = DominatedSucc == Successor1 ? Successor2 :
                                                             Successor1;
            if (DominatedSucc == Successor1)
              Then = findASTNode(AST, TileToNodeMap, Successor1);
            if (DominatedSucc == Successor2)
              Else = findASTNode(AST, TileToNodeMap, Successor2);
          }
          createTile(Graph, ASTDT, TileToNodeMap, Node, NotDominatedSucc);

          // Build the `IfNode`.
          using UniqueExpr = ASTTree::expr_unique_ptr;
          using ExprDestruct = ASTTree::expr_destructor;
          auto *OriginalNode = Node->getOriginalNode();
          UniqueExpr CondExpr(new AtomicNode(OriginalNode), ExprDestruct());
          ExprNode *Condition = AST.addCondExpr(std::move(CondExpr));

          // Insert the postdominator if the current tile actually has it.
          ASTObject.reset(new IfNode(Node, Condition, Then, Else, nullptr));
        } break;
        case 2: {

          // TODO: Handle this case.
          // This means that we have two successors, and we dominate both the
          // two successors, or one successor and the postdominator.
          revng_assert(not Node->isBreak() and not Node->isContinue()
                       and not Node->isSet());
          BasicBlockNode<NodeT> *Successor1 = Successors[0];
          BasicBlockNode<NodeT> *Successor2 = Successors[1];

          // First of all, check if one of the successors can also be
          // considered as the immediate postdominator of the tile.
          auto *SuccOfSucc1 = getDirectSuccessor(Successor1);
          auto *SuccOfSucc2 = getDirectSuccessor(Successor2);
          revng_assert(SuccOfSucc1 != SuccOfSucc2 or nullptr == SuccOfSucc1);

          using ConstEdge = std::pair<const BasicBlockNodeT *,
                                      const BasicBlockNodeT *>;
          bool Inlined1 = isEdgeInlined(ConstEdge{ Node, Successor1 });
          bool Inlined2 = isEdgeInlined(ConstEdge{ Node, Successor2 });

          ASTNode *Then = nullptr;
          ASTNode *Else = nullptr;
          BasicBlockNode<NodeT> *PostDomBB = nullptr;

          if (Inlined1 and Inlined2) {
            revng_assert(ASTDT.dominates(Node, Successor1));
            revng_assert(ASTDT.dominates(Node, Successor2));
            Then = findASTNode(AST, TileToNodeMap, Successor1);
            Else = findASTNode(AST, TileToNodeMap, Successor2);
          } else if (Inlined1) {
            revng_assert(ASTDT.dominates(Node, Successor1));
            Then = findASTNode(AST, TileToNodeMap, Successor1);
            PostDomBB = Successor2;
          } else if (Inlined2) {
            revng_assert(ASTDT.dominates(Node, Successor2));
            Else = findASTNode(AST, TileToNodeMap, Successor2);
            PostDomBB = Successor1;
          } else if (SuccOfSucc1 == Successor2) {
            revng_assert(SuccOfSucc2 != Successor1);
            revng_assert(ASTDT.dominates(Node, Successor1));
            Then = findASTNode(AST, TileToNodeMap, Successor1);
            PostDomBB = Successor2;
          } else if (SuccOfSucc2 == Successor1) {
            revng_assert(SuccOfSucc1 != Successor2);
            revng_assert(ASTDT.dominates(Node, Successor2));
            Else = findASTNode(AST, TileToNodeMap, Successor2);
            PostDomBB = Successor1;
          } else {
            revng_assert(ASTDT.dominates(Node, Successor1));
            revng_assert(ASTDT.dominates(Node, Successor2));
            Then = findASTNode(AST, TileToNodeMap, Successor1);
            Else = findASTNode(AST, TileToNodeMap, Successor2);
          }

          // Build the `IfNode`.
          using UniqueExpr = ASTTree::expr_unique_ptr;
          using ExprDestruct = ASTTree::expr_destructor;
          auto *OriginalNode = Node->getOriginalNode();
          UniqueExpr CondExpr(new AtomicNode(OriginalNode), ExprDestruct());
          ExprNode *Condition = AST.addCondExpr(std::move(CondExpr));

          // Insert the postdominator if the current tile actually has it.
          ASTNode *PostDom = nullptr;
          if (PostDomBB)
            PostDom = findASTNode(AST, TileToNodeMap, PostDomBB);

          ASTObject.reset(new IfNode(Node, Condition, Then, Else, PostDom));

          createTile(Graph, ASTDT, TileToNodeMap, Node, PostDomBB);
        } break;
        case 3: {

          // This is the standard situation, we have two successors, we
          // dominate both of them and we also dominate the postdominator
          // node.
          revng_assert(not Node->isBreak() and not Node->isContinue()
                       and not Node->isSet());

          // Check that our successor nodes are also in the dominated node
          // vector.
          BasicBlockNode<NodeT> *Successor1 = Successors[0];
          BasicBlockNode<NodeT> *Successor2 = Successors[1];
          revng_assert(containsSmallVector(Children, Successor1));
          revng_assert(containsSmallVector(Children, Successor2));

          ASTNode *Then = findASTNode(AST, TileToNodeMap, Successor1);
          ASTNode *Else = findASTNode(AST, TileToNodeMap, Successor2);

          // Retrieve the successors of the `then` and `else` nodes. We expect
          // the successor to be identical due to the structure of the tile we
          // are covering. And we expect it to be the PostDom node of the
          // tile.
          BasicBlockNode<NodeT> *PostDomBB = findCommonPostDom(Successor1,
                                                               Successor2);
          ASTNode *PostDom = nullptr;
          if (PostDomBB != nullptr) {
            // Check that the postdom is between the nodes dominated by the
            // current node.
            revng_assert(containsSmallVector(Children, PostDomBB));
            PostDom = findASTNode(AST, TileToNodeMap, PostDomBB);
          }

          // Build the `IfNode`.
          using UniqueExpr = ASTTree::expr_unique_ptr;
          using ExprDestruct = ASTTree::expr_destructor;
          auto *OriginalNode = Node->getOriginalNode();
          UniqueExpr CondExpr(new AtomicNode(OriginalNode), ExprDestruct());
          ExprNode *Condition = AST.addCondExpr(std::move(CondExpr));
          ASTObject.reset(new IfNode(Node, Condition, Then, Else, PostDom));

          createTile(Graph, ASTDT, TileToNodeMap, Node, PostDomBB);
        } break;

        default: {
          revng_log(CombLogger,
                    "Node: " << Node->getNameStr() << " dominates "
                             << Children.size() << " nodes");
          revng_unreachable("Node directly dominates more than 3 other "
                            "nodes");
        } break;
        }
      } break;

      case 1: {
        switch (Children.size()) {
        case 0: {

          // In this situation, we don't need to actually add as a successor
          // of the current node the single successor which is not dominated.
          // Therefore, the successor will not be a succesor on the AST.
          revng_assert(not Node->isBreak() and not Node->isContinue());
          if (Node->isSet()) {
            ASTObject.reset(new SetNode(Node));
          } else {
            ASTObject.reset(new CodeNode(Node, nullptr));
          }
        } break;

        case 1: {

          // In this situation, we dominate the only successor of the current
          // node. The successor therefore will be an actual successor on the
          // AST.
          revng_assert(not Node->isBreak() and not Node->isContinue());
          revng_assert(Successors[0] == Children[0]);
          auto *Succ = findASTNode(AST, TileToNodeMap, Children[0]);
          if (Node->isSet()) {
            ASTObject.reset(new SetNode(Node, Succ));
          } else {
            ASTObject.reset(new CodeNode(Node, Succ));
          }
          createTile(Graph, ASTDT, TileToNodeMap, Node, Children[0]);
        } break;

        default: {
          revng_log(CombLogger,
                    "Node: " << Node->getNameStr() << " dominates "
                             << Children.size() << "nodes");
          revng_unreachable("Node with 1 successor dominates an incorrect "
                            "number of nodes");
        } break;
        }
      } break;

      case 0: {
        if (Node->isBreak())
          ASTObject.reset(new BreakNode());
        else if (Node->isContinue())
          ASTObject.reset(new ContinueNode());
        else if (Node->isSet())
          ASTObject.reset(new SetNode(Node));
        else if (Node->isEmpty() or Node->isCode())
          ASTObject.reset(new CodeNode(Node, nullptr));
        else
          revng_abort();
      } break;

      default: {
        revng_log(CombLogger,
                  "Node: " << Node->getNameStr() << " dominates "
                           << Children.size() << " nodes");
        revng_unreachable("Node directly dominates more than 3 other nodes");
      } break;
      }
    }
    AST.addASTNode(Node, std::move(ASTObject));
  }

  // Set in the ASTTree object the root node.
  BasicBlockNode<NodeT> *Root = ASTDT.getRootNode()->getBlock();
  revng_assert(Root);
  ASTNode *RootNode = AST.findASTNode(Root);
  AST.setRoot(RootNode);
}

inline void normalize(ASTTree &AST, std::string FunctionName) {
  // Serialize the graph starting from the root node.
  CombLogger << "Serializing first AST draft:\n";
  if (CombLogger.isEnabled()) {
    AST.dumpOnFile("ast", FunctionName, "First-draft");
  }

  // Create sequence nodes.
  CombLogger << "Performing sequence insertion:\n";
  ASTNode *RootNode = AST.getRoot();
  RootNode = createSequence(AST, RootNode);
  AST.setRoot(RootNode);
  if (CombLogger.isEnabled()) {
    AST.dumpOnFile("ast", FunctionName, "After-sequence");
  }

  // Simplify useless sequence nodes.
  CombLogger << "Performing useless dummies simplification:\n";
  simplifyDummies(RootNode);
  if (CombLogger.isEnabled()) {
    AST.dumpOnFile("ast", FunctionName, "After-dummies-removal");
  }

  // Simplify useless sequence nodes.
  CombLogger << "Performing useless sequence simplification:\n";
  RootNode = simplifyAtomicSequence(AST, RootNode);
  AST.setRoot(RootNode);
  if (CombLogger.isEnabled()) {
    AST.dumpOnFile("ast", FunctionName, "After-sequence-simplification");
  }
}
#endif // REVNGC_RESTRUCTURE_CFG_GENERATEAST_H
