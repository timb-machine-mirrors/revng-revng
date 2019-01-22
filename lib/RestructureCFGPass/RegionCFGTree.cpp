/// \file RegionCFGTree.cpp
/// \brief

//
// This file is distributed under the MIT License. See LICENSE.md for details.
//

// Standard includes
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

// LLVM includes
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/GenericDomTreeConstruction.h"
#include "llvm/Support/raw_os_ostream.h"

// Local libraries includes
#include "revng-c/RestructureCFGPass/ASTTree.h"
#include "revng-c/RestructureCFGPass/RegionCFGTree.h"
#include "revng-c/RestructureCFGPass/Utils.h"

// EdgeDescriptor is a handy way to create and manipulate edges on the RegionCFG.
using EdgeDescriptor = std::pair<BasicBlockNode *, BasicBlockNode *>;

// Helper function that visit an AST tree and creates the sequence nodes
static ASTNode *createSequence(ASTTree &Tree, ASTNode *RootNode) {
  SequenceNode *RootSequenceNode = Tree.addSequenceNode();
  RootSequenceNode->addNode(RootNode);

  for (ASTNode *Node : RootSequenceNode->nodes()) {
    if (auto *If = llvm::dyn_cast<IfNode>(Node)) {
      If->setThen(createSequence(Tree, If->getThen()));
      If->setElse(createSequence(Tree, If->getElse()));
    }
    #if 0
  } else if (auto *Code = llvm::dyn_cast<CodeNode>(Node)) {
      // TODO: confirm that doesn't make sense to process a code node.
    } else if (auto *Scs = llvm::dyn_cast<ScsNode>(Node)) {
      // TODO: confirm that this phase is not needed since the processing is
      //       done inside the processing of each SCS region.
    }
    #endif
  }

return RootSequenceNode;
}

// Helper function that simplifies useless dummy nodes
static void simplifyDummies(ASTNode *RootNode) {

  if (auto *Sequence = llvm::dyn_cast<SequenceNode>(RootNode)) {
    std::vector<ASTNode *> UselessDummies;

    for (ASTNode *Node : Sequence->nodes()) {
      if (Node->isDummy()) {
        UselessDummies.push_back(Node);
      } else {
        simplifyDummies(Node);
      }
    }

    for (ASTNode *Node : UselessDummies) {
      Sequence->removeNode(Node);
    }

  } else if (auto *If = llvm::dyn_cast<IfNode>(RootNode)) {
    simplifyDummies(If->getThen());
    simplifyDummies(If->getElse());
  }
}

// Helper function which simplifies sequence nodes composed by a single AST
// node.
static ASTNode *simplifyAtomicSequence(ASTNode *RootNode) {
  if (auto *Sequence = llvm::dyn_cast<SequenceNode>(RootNode)) {
    if (Sequence->listSize() == 0) {
      RootNode = nullptr;
    } else if (Sequence->listSize() == 1) {
      RootNode = Sequence->getNodeN(0);
      RootNode = simplifyAtomicSequence(RootNode);
    } else {
      for (ASTNode *Node : Sequence->nodes()) {
        Node = simplifyAtomicSequence(Node);
      }
    }
  } else if (auto *If = llvm::dyn_cast<IfNode>(RootNode)) {
    If->setThen(simplifyAtomicSequence(If->getThen()));
    If->setElse(simplifyAtomicSequence(If->getElse()));
  }
  #if 0
} else if (auto *Scs = llvm::dyn_cast<ScsNode>(RootNode)) {
    // TODO: check if this is not needed as the simplification is done for each
    //       SCS region.
  }
  #endif

  return RootNode;
}

// Helper function to simplify short-circuit IFs
static void simplifyShortCircuit(ASTNode *RootNode) {

  if (auto *Sequence = llvm::dyn_cast<SequenceNode>(RootNode)) {
    for (ASTNode *Node : Sequence->nodes()) {
      simplifyShortCircuit(Node);
    }
  } else if (auto *Scs = llvm::dyn_cast<ScsNode>(RootNode)) {
    simplifyShortCircuit(Scs->getBody());
  } else if (auto *If = llvm::dyn_cast<IfNode>(RootNode)) {
    if (If->hasBothBranches()) {

      if (auto InternalIf = llvm::dyn_cast<IfNode>(If->getThen())) {

        // TODO: Refactor this with some kind of iterator
        if (InternalIf->getThen() != nullptr) {
          if (If->getElse()->isEqual(InternalIf->getThen())) {
            CombLogger << "Candidate for short-circuit reduction found:\n";
            CombLogger << "IF " << If->getName() << " and ";
            CombLogger << "IF " << InternalIf->getName() << "\n";
            CombLogger << "Nodes being simplified:\n";
            CombLogger << If->getElse()->getName() << " and ";
            CombLogger << InternalIf->getThen()->getName() << "\n";

            If->setThen(InternalIf->getElse());
            If->setElse(InternalIf->getThen());
            simplifyShortCircuit(If);
          }
        }

        if (InternalIf->getElse() != nullptr) {
          if (If->getElse()->isEqual(InternalIf->getElse())) {
            CombLogger << "Candidate for short-circuit reduction found:\n";
            CombLogger << "IF " << If->getName() << " and ";
            CombLogger << "IF " << InternalIf->getName() << "\n";
            CombLogger << "Nodes being simplified:\n";
            CombLogger << If->getElse()->getName() << " and ";
            CombLogger << InternalIf->getElse()->getName() << "\n";

            If->setThen(InternalIf->getThen());
            If->setElse(InternalIf->getElse());
            simplifyShortCircuit(If);
          }
        }
      }

      if (auto InternalIf = llvm::dyn_cast<IfNode>(If->getElse())) {

        // TODO: Refactor this with some kind of iterator
        if (InternalIf->getThen() != nullptr) {
          if (If->getThen()->isEqual(InternalIf->getThen())) {
            CombLogger << "Candidate for short-circuit reduction found:\n";
            CombLogger << "IF " << If->getName() << " and ";
            CombLogger << "IF " << InternalIf->getName() << "\n";
            CombLogger << "Nodes being simplified:\n";
            CombLogger << If->getThen()->getName() << " and ";
            CombLogger << InternalIf->getThen()->getName() << "\n";

            If->setElse(InternalIf->getElse());
            If->setThen(InternalIf->getThen());
            simplifyShortCircuit(If);
          }
        }

        if (InternalIf->getElse() != nullptr) {
          if (If->getThen()->isEqual(InternalIf->getElse())) {
            CombLogger << "Candidate for short-circuit reduction found:\n";
            CombLogger << "IF " << If->getName() << " and ";
            CombLogger << "IF " << InternalIf->getName() << "\n";
            CombLogger << "Nodes being simplified:\n";
            CombLogger << If->getThen()->getName() << " and ";
            CombLogger << InternalIf->getElse()->getName() << "\n";

            If->setElse(InternalIf->getThen());
            If->setThen(InternalIf->getElse());
            simplifyShortCircuit(If);
          }
        }
      }

    }
  }
}

static void flipEmptyThen(ASTNode *RootNode) {
  if (auto *Sequence = llvm::dyn_cast<SequenceNode>(RootNode)) {
    for (ASTNode *Node : Sequence->nodes()) {
      flipEmptyThen(Node);
    }
  } else if (auto *If = llvm::dyn_cast<IfNode>(RootNode)) {
    if (!If->hasThen()) {
      if (CombLogger.isEnabled()) {
        CombLogger << "Flipping then and else branches for : ";
        CombLogger << If->getName() << "\n";
      }
      If->setThen(If->getElse());
      If->setElse(nullptr);
      flipEmptyThen(If->getThen());
    } else {

      // We are sure to have the `then` branch since the previous check did
      // not verify
      flipEmptyThen(If->getThen());

      // We have not the same assurance for the `else` branch
      if (If->hasElse()) {
        flipEmptyThen(If->getElse());
      }
    }
  } else if (auto *Scs = llvm::dyn_cast<ScsNode>(RootNode)) {
    flipEmptyThen(Scs->getBody());
  }
}

void RegionCFG::initialize(llvm::Function &F) {

  // Create a new node for each basic block in the module.
  for (llvm::BasicBlock &BB : F) {
    addNode(&BB);
  }

  // Set entry node references.
  Entry = &(F.getEntryBlock());
  EntryNode = &(get(Entry));

  // Connect each node to its successors.
  for (llvm::BasicBlock &BB : F) {
    BasicBlockNode &Node = get(&BB);

    llvm::TerminatorInst *Terminator = BB.getTerminator();
    int SuccessorNumber = Terminator->getNumSuccessors();

    if (SuccessorNumber < 3) {
      // Add the successors to the node.
      for (llvm::BasicBlock *Successor : Terminator->successors()) {
        BasicBlockNode &SuccessorNode = get(Successor);
        Node.addSuccessor(&SuccessorNode);
        SuccessorNode.addPredecessor(&Node);
      }
    } else {

      // HACK: handle switches as a nested tree of ifs.
      std::vector<llvm::BasicBlock *> WorkList;
      for (llvm::BasicBlock *Successor : reverse(Terminator->successors())) {
        WorkList.push_back(Successor);
      }

      BasicBlockNode *PrevDummy = &get(&BB);

      // For each iteration except the last create a new dummy node
      // connecting the successors.
      while (WorkList.size() > 2) {
        BasicBlockNode *NewDummy = addDummyNode("switch dummy");
        BasicBlockNode *Dest1 = &get(WorkList.back());
        WorkList.pop_back();
        addEdge(EdgeDescriptor(PrevDummy, Dest1));
        addEdge(EdgeDescriptor(PrevDummy, NewDummy));
        PrevDummy = NewDummy;
      }

      BasicBlockNode *Dest1 = &get(WorkList.back());
      WorkList.pop_back();
      BasicBlockNode *Dest2 = &get(WorkList.back());
      WorkList.pop_back();
      revng_assert(WorkList.empty());
      addEdge(EdgeDescriptor(PrevDummy, Dest1));
      addEdge(EdgeDescriptor(PrevDummy, Dest2));
    }

    // Set as return block if there are no successors.
    if (Terminator->getNumSuccessors() == 0) {
      Node.setReturn();
    }
  }
}

void RegionCFG::addNode(llvm::BasicBlock *BB) {
  BlockNodes.emplace_back(std::make_unique<BasicBlockNode>(this, BB));
  BBMap[BB] = BlockNodes.back().get();
  CombLogger << "Building " << BB->getName();
  CombLogger << " at address: " << BBMap[BB] << "\n";
}

BasicBlockNode *RegionCFG::cloneNode(const BasicBlockNode &OriginalNode) {
  BlockNodes.emplace_back(std::make_unique<BasicBlockNode>(OriginalNode));
  BasicBlockNode *New = BlockNodes.back().get();
  New->setName(std::string(OriginalNode.getName()) + "cloned");
  return New;
}

void RegionCFG::removeNode(BasicBlockNode *Node) {

  CombLogger << "Removing node named: " << Node->getNameStr() << "\n";

  for (BasicBlockNode *Predecessor : Node->predecessors()) {
    Predecessor->removeSuccessor(Node);
  }

  for (BasicBlockNode *Successor : Node->successors()) {
    Successor->removePredecessor(Node);
  }

  for (auto It = BlockNodes.begin(); It != BlockNodes.end(); It++) {
    if ((*It).get() == Node) {
      BlockNodes.erase(It);
      break;
    }
  }
}

void
RegionCFG::insertBulkNodes(std::set<BasicBlockNode *> &Nodes,
                           BasicBlockNode *Head,
                           RegionCFG::BBNodeMap &SubstitutionMap) {
  BlockNodes.clear();

  for (BasicBlockNode *Node : Nodes) {
    BlockNodes.emplace_back(std::make_unique<BasicBlockNode>(*Node));
    BasicBlockNode *New = BlockNodes.back().get();
    // The copy constructor used above does not bring along the successors and
    // the predecessors, neither adjusts the parent.
    // The following lines are a hack to fix this problem, but they momentarily
    // build a broken data structure where the predecessors and the successors
    // of the New BasicBlockNodes in *this still refer to the BasicBlockNodes in
    // the Parent CFGRegion of Nodes. This will be fixed later by updatePointers
    New->setParent(this);
    for (BasicBlockNode *Succ : Node->successors())
      New->addSuccessor(Succ);
    for (BasicBlockNode *Pred : Node->predecessors())
      New->addPredecessor(Pred);
    SubstitutionMap[Node] = New;
  }

  revng_assert(Head != nullptr);
  EntryNode = SubstitutionMap[Head];
  revng_assert(EntryNode != nullptr);
  // Fix the hack above
  for (std::unique_ptr<BasicBlockNode> &Node : BlockNodes)
    Node->updatePointers(SubstitutionMap);
}

void
RegionCFG::connectBreakNode(std::set<EdgeDescriptor> &Outgoing,
                            BasicBlockNode *Break,
                            const BBNodeMap &SubstitutionMap) {
  for (EdgeDescriptor Edge : Outgoing)
    addEdge(EdgeDescriptor(SubstitutionMap.at(Edge.first), Break));
}

void RegionCFG::connectContinueNode(BasicBlockNode *Continue) {
  for (BasicBlockNode *Source : EntryNode->predecessors()) {
    moveEdgeTarget(EdgeDescriptor(Source, EntryNode), Continue);
  }
}

BasicBlockNode &RegionCFG::get(llvm::BasicBlock *BB) {
  auto It = BBMap.find(BB);
  revng_assert(It != BBMap.end());
  return *(It->second);
}

BasicBlockNode &RegionCFG::getRandomNode() {
  int randNum = rand() % (BBMap.size());
  auto randomIt = std::next(std::begin(BBMap), randNum);
  return *(randomIt->second);
}

std::vector<BasicBlockNode *> RegionCFG::orderNodes(std::vector<BasicBlockNode *> &L,
                                              bool DoReverse) {
  llvm::ReversePostOrderTraversal<BasicBlockNode *> RPOT(EntryNode);
  std::vector<BasicBlockNode *> Result;

  if (DoReverse) {
    std::reverse(RPOT.begin(), RPOT.end());
  }

  CombLogger << "New ordering" << "\n";
  for (BasicBlockNode *Node : L) {
    CombLogger << Node->getNameStr() << "\n";
    CombLogger.emit();
  }

  for (BasicBlockNode *RPOTBB : RPOT) {
    for (BasicBlockNode *Node : L) {
      if (RPOTBB == Node) {
        Result.push_back(Node);
      }
    }
  }

  revng_assert(L.size() == Result.size());

  return Result;
}

template<typename StreamT>
void RegionCFG::streamNode(StreamT &S, const BasicBlockNode *BB) const {
  unsigned NodeID = BB->getID();
  S << "\"" << NodeID << "\"";
  S << " [" << "label=\"ID: " << NodeID << " Name: " << BB->getName().str() << "\"";
  if (BB == EntryNode)
    S << ",fillcolor=green,style=filled";
  if (BB->isReturn())
    S << ",fillcolor=red,style=filled";
  S << "];\n";
}

/// \brief Dump a GraphViz file on stdout representing this function
template<typename StreamT>
void RegionCFG::dumpDot(StreamT &S) {
  S << "digraph CFGFunction {\n";

  for (std::unique_ptr<BasicBlockNode> &BB : BlockNodes) {
    streamNode(S, BB.get());
    for (auto &Successor : BB->successors()) {
      unsigned PredID = BB->getID();
      unsigned SuccID = Successor->getID();
      S << "\"" << PredID << "\"" << " -> \"" << SuccID << "\""
        << " [color=green];\n";
    }
  }
  S << "}\n";
}

void RegionCFG::dumpDotOnFile(std::string FunctionName, std::string FileName) {
  std::ofstream DotFile;
  std::string PathName = "dots/" + FunctionName;
  mkdir(PathName.c_str(), 0775);
  DotFile.open("dots/" + FunctionName + "/" + FileName + ".dot");
  dumpDot(DotFile);
}

void RegionCFG::purgeDummies() {
  RegionCFG &Graph = *this;
  bool AnotherIteration = true;

  while (AnotherIteration) {
    AnotherIteration = false;

    for (auto It = Graph.begin(); It != Graph.end(); It++) {
      if (((*It)->isDummy())
          and ((*It)->predecessor_size() == 1)
          and ((*It)->successor_size() == 1)) {

        if (CombLogger.isEnabled()) {
          CombLogger << "Purging dummy node " << (*It)->getNameStr() << "\n";
        }

        BasicBlockNode *Predecessor = (*It)->getPredecessorI(0);
        BasicBlockNode *Successor = (*It)->getSuccessorI(0);

        moveEdgeTarget(EdgeDescriptor(Predecessor, (*It)), Successor);
        removeEdge(EdgeDescriptor((*It), Successor));
        Graph.removeNode(*It);
        AnotherIteration = true;
        break;
      }
    }
  }
}

void RegionCFG::purgeVirtualSink(BasicBlockNode *Sink) {

  RegionCFG &Graph = *this;

  std::vector<BasicBlockNode *> WorkList;
  std::vector<BasicBlockNode *> PurgeList;

  WorkList.push_back(Sink);

  while (!WorkList.empty()) {
    BasicBlockNode *CurrentNode = WorkList.back();
    WorkList.pop_back();

    if (CurrentNode->isDummy()) {
      PurgeList.push_back(CurrentNode);

      for (BasicBlockNode *Predecessor : CurrentNode->predecessors()) {
        WorkList.push_back(Predecessor);
      }
    }
  }

  for (BasicBlockNode *Purge : PurgeList) {
    Graph.removeNode(Purge);
  }
}

std::vector<BasicBlockNode *> RegionCFG::getInterestingNodes(BasicBlockNode *Cond) {

  RegionCFG &Graph = *this;

  llvm::DominatorTreeBase<BasicBlockNode, false> DT;
  DT.recalculate(Graph);
  llvm::DominatorTreeBase<BasicBlockNode, true> PDT;
  PDT.recalculate(Graph);

  // Retrieve the immediate postdominator.
  llvm::DomTreeNodeBase<BasicBlockNode> *PostBase = PDT[Cond]->getIDom();
  BasicBlockNode *PostDominator = PostBase->getBlock();

  std::set<BasicBlockNode *> Candidates = findReachableNodes(*Cond,
                                                             *PostDominator);

  std::vector<BasicBlockNode *> NotDominatedCandidates;
  for (BasicBlockNode *Node : Candidates) {
    if (!DT.dominates(Cond, Node)) {
      NotDominatedCandidates.push_back(Node);
    }
  }

  // TODO: Check that this is the order that we want.
  NotDominatedCandidates = Graph.orderNodes(NotDominatedCandidates, true);

  return NotDominatedCandidates;
}

void RegionCFG::inflate() {

  // Apply the comb to a RegionCFG object.
  // TODO: handle all the collapsed regions.
  RegionCFG &Graph = *this;

  // Refresh information of dominator and postdominator trees.
  llvm::DominatorTreeBase<BasicBlockNode, false> DT;
  DT.recalculate(Graph);

  llvm::DominatorTreeBase<BasicBlockNode, true> PDT;
  PDT.recalculate(Graph);

  // Collect entry and exit nodes.
  BasicBlockNode *EntryNode = &Graph.getEntryNode();
  std::vector<BasicBlockNode *> ExitNodes;
  for (auto It = Graph.begin(); It != Graph.end(); It++) {
    if ((*It)->successor_size() == 0) {
      ExitNodes.push_back(*It);
    }
  }

  if (CombLogger.isEnabled()) {
    CombLogger << "The entry node is:\n";
    CombLogger << EntryNode->getNameStr() << "\n";
    CombLogger << "In the graph the exit nodes are:\n";
    for (BasicBlockNode *Node : ExitNodes) {
      CombLogger << Node->getNameStr() << "\n";
    }
  }

  // Add a new virtual sink node to which all the exit nodes are connected.
  BasicBlockNode *Sink = Graph.addDummyNode("Virtual sink");
  for (BasicBlockNode *Exit : ExitNodes) {
    addEdge(EdgeDescriptor(Exit, Sink));
  }

  // Dump graph after virtual sink add.
  if (CombLogger.isEnabled()) {
    CombLogger << "Graph after sink addition is:\n";
    Graph.dumpDot(CombLogger);
  }

  // Collect all the conditional nodes in the graph.
  std::vector<BasicBlockNode *> ConditionalNodes;
  for (auto It = Graph.begin(); It != Graph.end(); It++) {
    revng_assert((*It)->successor_size() < 3);
    if ((*It)->successor_size() == 2) {
      ConditionalNodes.push_back(*It);
    }
  }

  // TODO: reverse this order, with std::vector I can only pop_back.
  ConditionalNodes = Graph.orderNodes(ConditionalNodes, false);

  if (CombLogger.isEnabled()) {
    CombLogger << "Conditional nodes present in the graph are:\n";
    for (BasicBlockNode *Node : ConditionalNodes) {
      CombLogger << Node->getNameStr() << "\n";
    }
  }

  while (!ConditionalNodes.empty()) {

    // Process each conditional node after ordering it.
    BasicBlockNode *Conditional = ConditionalNodes.back();
    ConditionalNodes.pop_back();
    if (CombLogger.isEnabled()) {
      CombLogger << "Analyzing conditional node " << Conditional->getNameStr()
                 << "\n";

    }
    Graph.dumpDot(CombLogger);
    CombLogger.emit();

    // Update information of dominator and postdominator trees.
    DT.recalculate(Graph);
    PDT.recalculate(Graph);

    // Get all the nodes reachable from the current conditional node (stopping
    // at the immediate postdominator) and that we want to duplicate/split.
    std::vector<BasicBlockNode *> NotDominatedCandidates;
    NotDominatedCandidates = getInterestingNodes(Conditional);

    while (!NotDominatedCandidates.empty()) {
      if (CombLogger.isEnabled()) {
        CombLogger << "Analyzing candidate nodes\n ";
      }
      DT.recalculate(Graph);
      BasicBlockNode *Candidate = NotDominatedCandidates.back();
      NotDominatedCandidates.pop_back();
      if (CombLogger.isEnabled()) {
        CombLogger << "Analyzing candidate " << Candidate->getNameStr()
                   << "\n";
      }
      Graph.dumpDot(CombLogger);
      CombLogger.emit();

      // Decide wether to insert a dummy or to duplicate.
      if (Candidate->predecessor_size() > 2) {

        // Insert a dummy node.
        if (CombLogger.isEnabled()) {
          CombLogger << "Inserting a dummy node for ";
          CombLogger << Candidate->getNameStr() << "\n";
        }

        typedef enum {Left, Right} Side;

        std::vector<Side> Sides{Left, Right};
        std::map<Side, BasicBlockNode *> Dummies;

        for (Side S : Sides) {
          BasicBlockNode *Dummy = Graph.addDummyNode("dummy");
          Dummies[S] = Dummy;
        }

        std::vector<BasicBlockNode *> Predecessors;

        CombLogger << "Current predecessors are:\n";
        for (BasicBlockNode *Predecessor : Candidate->predecessors()) {
          CombLogger << Predecessor->getNameStr() << "\n";
          Predecessors.push_back(Predecessor);
        }

        for (BasicBlockNode *Predecessor : Predecessors) {
          if (CombLogger.isEnabled()) {
            CombLogger << "Moving edge from predecessor ";
            CombLogger << Predecessor->getNameStr() << "\n";
          }
          if (DT.dominates(Conditional, Predecessor)) {
            moveEdgeTarget(EdgeDescriptor(Predecessor, Candidate),
                           Dummies[Left]);
          } else {
            moveEdgeTarget(EdgeDescriptor(Predecessor, Candidate),
                           Dummies[Right]);
          }
        }

        for (Side S : Sides) {
          addEdge(EdgeDescriptor(Dummies[S], Candidate));
        }

        NotDominatedCandidates = getInterestingNodes(Conditional);
      } else {

        // Duplicate node.
        if (CombLogger.isEnabled()) {
          CombLogger << "Duplicating node for ";
          CombLogger << Candidate->getNameStr() << "\n";
        }


        // TODO: change this using a clone like method of BasicBlockNode that
        //       preserves the dummy information.
        BasicBlockNode *Duplicated;
        if (Candidate->isDummy()) {
          std::string NodeName = Candidate->getNameStr() + " duplicated";
          Duplicated = Graph.addDummyNode(NodeName);
        } else {
          Duplicated = Graph.cloneNode(*Candidate);
        }

        assert(Duplicated != nullptr);

        for (BasicBlockNode *Successor : Candidate->successors()) {
          addEdge(EdgeDescriptor(Duplicated, Successor));
        }

        std::vector<BasicBlockNode *> Predecessors;

        for (BasicBlockNode *Predecessor : Candidate->predecessors()) {
          Predecessors.push_back(Predecessor);
        }

        for (BasicBlockNode *Predecessor : Predecessors) {
          if (!DT.dominates(Conditional, Predecessor)) {
            moveEdgeTarget(EdgeDescriptor(Predecessor, Candidate),
                           Duplicated);
          }
        }
      }

      // Refresh the info on candidates.
      NotDominatedCandidates = getInterestingNodes(Conditional);
    }
  }

  // Purge extra dummy nodes introduced.
  purgeDummies();
  purgeVirtualSink(Sink);

  if (CombLogger.isEnabled()) {
    CombLogger << "Graph after combing is:\n";
    Graph.dumpDot(CombLogger);
  }
}

ASTNode *RegionCFG::generateAst() {

  RegionCFG &Graph = *this;

  // Apply combing to the current RegionCFG.
  CombLogger << "Inflating region\n";
  Graph.inflate();

  // TODO: factorize out the AST generation phase.
  llvm::DominatorTreeBase<BasicBlockNode, false> DT;
  DT.recalculate(Graph);
#if 0
  llvm::raw_os_ostream Stream(dbg);
#endif
  DT.updateDFSNumbers();
#if 0
  DT.print(Stream);
  Stream.flush();
  DT.print(CombLogger);
#endif

  CombLogger.emit();

  std::map<int, BasicBlockNode *> DFSNodeMap;

  // Compute the ideal order of visit for creating AST nodes.
  for (BasicBlockNode *Node : Graph.nodes()) {
    DFSNodeMap[DT[Node]->getDFSNumOut()] = Node;
  }

  // Visiting order of the dominator tree.
  if (CombLogger.isEnabled()) {
    for (auto &Pair : DFSNodeMap) {
      CombLogger << Pair.second->getNameStr() << "\n";
    }
  }

  for (auto &Pair : DFSNodeMap) {
    BasicBlockNode *Node = Pair.second;

    // Collect the children nodes in the dominator tree.
    std::vector<llvm::DomTreeNodeBase<BasicBlockNode> *> Children =
      DT[Node]->getChildren();

    std::vector<ASTNode *> ASTChildren;
    for (llvm::DomTreeNodeBase<BasicBlockNode> *TreeNode : Children) {
      BasicBlockNode *BlockNode = TreeNode->getBlock();
      ASTNode *ASTPointer = AST.findASTNode(BlockNode);
      ASTChildren.push_back(ASTPointer);
    }

    // Check that the two vector have the same size.
    revng_assert(Children.size() == ASTChildren.size());

    // Handle collapsded node.
    if (Node->isCollapsed()) {
      revng_assert(ASTChildren.size() <= 1);
      if (ASTChildren.size() == 1) {
        RegionCFG *BodyGraph = Node->getCollapsedCFG();
        revng_assert(BodyGraph != nullptr);
        CombLogger << "Inspecting collapsed node: " << Node->getNameStr()
                   << "\n";
        CombLogger.emit();
        ASTNode *Body = BodyGraph->generateAst();
        std::unique_ptr<ASTNode> ASTObject(new ScsNode(Node,
                                                       Body,
                                                       ASTChildren[0]));
        AST.addASTNode(Node, std::move(ASTObject));
      } else {
        RegionCFG *BodyGraph = Node->getCollapsedCFG();
        CombLogger << "Inspecting collapsed node: " << Node->getNameStr()
                   << "\n";
        CombLogger.emit();
        ASTNode *Body = BodyGraph->generateAst();
        std::unique_ptr<ASTNode> ASTObject(new ScsNode(Node, Body));
        AST.addASTNode(Node, std::move(ASTObject));
      }
    } else {
      revng_assert(Children.size() < 4);
      if (Children.size() == 3) {
        std::unique_ptr<ASTNode> ASTObject(new IfNode(Node,
                                                      ASTChildren[0],
                                                      ASTChildren[2],
                                                      ASTChildren[1]));
        AST.addASTNode(Node, std::move(ASTObject));
      } else if (Children.size() == 2) {
        std::unique_ptr<ASTNode> ASTObject(new IfNode(Node,
                                                      ASTChildren[0],
                                                      ASTChildren[1],
                                                      nullptr));
        AST.addASTNode(Node, std::move(ASTObject));
      } else if (Children.size() == 1) {
        std::unique_ptr<ASTNode> ASTObject(new CodeNode(Node,
                                                        ASTChildren[0]));
        AST.addASTNode(Node, std::move(ASTObject));
      } else if (Children.size() == 0) {
        std::unique_ptr<ASTNode> ASTObject(new CodeNode(Node, nullptr));
        AST.addASTNode(Node, std::move(ASTObject));
      }
    }
  }

  // Serialize the graph starting from the root node.
  BasicBlockNode *Root = DT.getRootNode()->getBlock();
  ASTNode *RootNode = AST.findASTNode(Root);

  if (CombLogger.isEnabled()) {
    CombLogger << "First AST draft is:\n";
    CombLogger << "digraph CFGFunction {\n";
    dumpNode(RootNode);
    CombLogger << "}\n";
  }

  // Create sequence nodes.
  RootNode = createSequence(AST, RootNode);

  if (CombLogger.isEnabled()) {
    CombLogger << "AST after sequence insertion:\n";
    CombLogger << "digraph CFGFunction {\n";
    dumpNode(RootNode);
    CombLogger << "}\n";
  }

  // Simplify useless sequence nodes.
  simplifyDummies(RootNode);

  if (CombLogger.isEnabled()) {
    CombLogger << "AST after useless dummies simplification:\n";
    CombLogger << "digraph CFGFunction {\n";
    dumpNode(RootNode);
    CombLogger << "}\n";
  }

  // Simplify useless sequence nodes.
  RootNode = simplifyAtomicSequence(RootNode);

  if (CombLogger.isEnabled()) {
    CombLogger << "AST after useless sequence simplification:\n";
    CombLogger << "digraph CFGFunction {\n";
    dumpNode(RootNode);
    CombLogger << "}\n";
  }

  // Flip IFs with empty then branches.
  flipEmptyThen(RootNode);

  if (CombLogger.isEnabled()) {
    CombLogger << "AST after flipping IFs with empty then branches\n";
    CombLogger << "digraph CFGFunction {\n";
    dumpNode(RootNode);
    CombLogger << "}\n";
  }

  // Simplify short-circuit nodes.
  CombLogger << "Performing short-circuit simplification\n";
  simplifyShortCircuit(RootNode);
  if (CombLogger.isEnabled()) {
    CombLogger << "AST after short-circuit simplification:\n";
    CombLogger << "digraph CFGFunction {\n";
    dumpNode(RootNode);
    CombLogger << "}\n";
  }

  return RootNode;
}

// Get reference to the AST object which is inside the RegionCFG object
ASTTree &RegionCFG::getAST() {
  return AST;
}
