/// XXX add licence
//

#ifndef _NODE_H_
#define _NODE_H_

#include "DGParameters.h"
#include "analysis/Analysis.h"

namespace dg {

template <typename NodeT>
class DependenceGraph;

/// --------------------------------------------------------
//  -- Node
//     one node in DependenceGraph
/// --------------------------------------------------------
template <typename DG, typename KeyT, typename NodeT>
class Node
{
public:
    typedef EdgesContainer<NodeT> ControlEdgesT;
    typedef EdgesContainer<NodeT> DependenceEdgesT;

    // to be able to reference the KeyT
    typedef KeyT KeyType;

    typedef typename ControlEdgesT::iterator control_iterator;
    typedef typename ControlEdgesT::const_iterator const_control_iterator;
    typedef typename DependenceEdgesT::iterator data_iterator;
    typedef typename DependenceEdgesT::const_iterator const_data_iterator;

    Node<DG, KeyT, NodeT>(const KeyT& k, DG *dg = nullptr)
        : key(k), parameters(nullptr), dg(static_cast<void *>(dg))
#if ENABLE_CFG
         , basicBlock(nullptr), nextNode(nullptr),
           prevNode(nullptr)
#endif
    {
        if (dg)
            dg->addNode(static_cast<NodeT *>(this));
    }

    void removeFromDG()
    {
        DG *tmp = static_cast<DG *>(dg);
        tmp->removeNode(key);
    }

    void *setDG(void *dg)
    {
        void *old = this->dg;
        this->dg = dg;

        return old;
    }

    DG *getDG() const
    {
        return static_cast<DG *>(dg);
    }

    bool addControlDependence(NodeT * n)
    {
        bool ret1, ret2;

        ret1 = n->revControlDepEdges.insert(static_cast<NodeT *>(this));
        ret2 = controlDepEdges.insert(n);

        // we either have both edges or none
        assert(ret1 == ret2);

        return ret2;
    }

    bool addDataDependence(NodeT * n)
    {
        bool ret1, ret2;

        ret1 = n->revDataDepEdges.insert(static_cast<NodeT *>(this));
        ret2 = dataDepEdges.insert(n);

        assert(ret1 == ret2);

        return ret2;
    }

    bool removeDataDependence(NodeT * n)
    {
        bool ret1, ret2;

        ret1 = n->revDataDepEdges.erase(static_cast<NodeT *>(this));
        ret2 = dataDepEdges.erase(n);

        // must have both or none
        assert(ret1 == ret2 && "Dep. edge without rev. or vice versa");

        return ret1;
    }

    bool removeControlDependence(NodeT * n)
    {
        bool ret1, ret2;

        ret1 = n->revControlDepEdges.erase(static_cast<NodeT *>(this));
        ret2 = controlDepEdges.erase(n);

        // must have both or none
        assert(ret1 == ret2 && "Control edge without rev. or vice versa");

        return ret1;
    }

    void removeOutcomingDDs()
    {
        for (auto dd : dataDepEdges)
            removeDataDependence(dd);
    }

    void removeIncomingDDs()
    {
        for (auto dd : revDataDepEdges)
            dd->removeDataDependence(static_cast<NodeT *>(this));
    }

    // remove all data dependencies going from/to this node
    void removeDDs()
    {
        removeOutcomingDDs();
        removeIncomingDDs();
    }

    // remove all control dependencies going from/to this node
    void removeOutcomingCDs()
    {
        for (auto cd : controlDepEdges)
            removeControlDependence(cd);
    }

    void removeIncomingCDs()
    {
        for (auto cd : revControlDepEdges)
            cd->removeControlDependence(static_cast<NodeT *>(this));
    }

    void removeCDs()
    {
        removeOutcomingCDs();
        removeIncomingCDs();
    }

    // remove all edges from/to this node
    void isolate()
    {
        // remove CD and DD from this node
        removeDDs();
        removeCDs();

        /// interconnect neighbours in CFG
#ifdef ENABLE_CFG
        // if this is head or tail of BB,
        // we must take it into account
        if (basicBlock) {
            // if this is the head of block, make
            // the next node head of the block
            if (basicBlock->getFirstNode() == this) {
                basicBlock->setFirstNode(nextNode);
                if (nextNode)
                    nextNode->setBasicBlock(basicBlock);
            }

            if (basicBlock->getLastNode() == this) {
                basicBlock->setLastNode(prevNode);
                if (prevNode)
                    prevNode->setBasicBlock(basicBlock);
            }

            // if this was the only node in BB, remove the BB
            if (basicBlock->getFirstNode() == nullptr) {
                assert(basicBlock->getLastNode() == nullptr);
                basicBlock->remove();
            }

            basicBlock = nullptr;

            // also, if this is a callSite,
            // it is no longer part of BBlock,
            // so we must remove it from callSites
            if (hasSubgraphs()) {
                bool ret = basicBlock->removeCallSite(static_cast<NodeT *>(this));
                assert(ret && "the call site was not in BB's callSites");
            }
        }

        // make previous node point to nextNode
        if (prevNode)
            prevNode->nextNode = nextNode;

        // make next node point to prevNode
        if (nextNode)
            nextNode->prevNode = prevNode;

        // no dangling reference, please
        prevNode = nextNode = nullptr;
#endif
    }

    // control dependency edges iterators
    control_iterator control_begin(void) { return controlDepEdges.begin(); }
    const_control_iterator control_begin(void) const { return controlDepEdges.begin(); }
    control_iterator control_end(void) { return controlDepEdges.end(); }
    const_control_iterator control_end(void) const { return controlDepEdges.end(); }

    // reverse control dependency edges iterators
    control_iterator rev_control_begin(void) { return revControlDepEdges.begin(); }
    const_control_iterator rev_control_begin(void) const { return revControlDepEdges.begin(); }
    control_iterator rev_control_end(void) { return revControlDepEdges.end(); }
    const_control_iterator rev_control_end(void) const { return revControlDepEdges.end(); }

    // data dependency edges iterators
    data_iterator data_begin(void) { return dataDepEdges.begin(); }
    const_data_iterator data_begin(void) const { return dataDepEdges.begin(); }
    data_iterator data_end(void) { return dataDepEdges.end(); }
    const_data_iterator data_end(void) const { return dataDepEdges.end(); }

    // reverse data dependency edges iterators
    data_iterator rev_data_begin(void) { return revDataDepEdges.begin(); }
    const_data_iterator rev_data_begin(void) const { return revDataDepEdges.begin(); }
    data_iterator rev_data_end(void) { return revDataDepEdges.end(); }
    const_data_iterator rev_data_end(void) const { return revDataDepEdges.end(); }

    unsigned int getControlDependenciesNum() const { return controlDepEdges.size(); }
    unsigned int getRevControlDependenciesNum() const { return revControlDepEdges.size(); }
    unsigned int getDataDependenciesNum() const { return dataDepEdges.size(); }
    unsigned int getRevDataDependenciesNum() const { return revDataDepEdges.size(); }

#ifdef ENABLE_CFG
    BBlock<NodeT> *getBasicBlock() { return basicBlock; }
    const BBlock<NodeT> *getBasicBlock() const { return basicBlock; }

    BBlock<NodeT> *setBasicBlock(BBlock<NodeT> *nbb)
    {
        BBlock<NodeT> *old = basicBlock;
        basicBlock = nbb;
        return old;
    }

    NodeT * setSuccessor(NodeT * s)
    {
        NodeT * old = nextNode;
        nextNode = s;

        assert(s != static_cast<NodeT *>(this)
                && "creating self-loop");

        s->prevNode = static_cast<NodeT *>(this);

        // set the same basic block for the next node
        // so that we don't have to do it manually
        // after setting the successor
        assert(basicBlock && "No basic block while setting succ");
        nextNode->setBasicBlock(basicBlock);

        return old;
    }

    bool hasSuccessor() const { return nextNode != nullptr; }
    bool hasPredcessor() const { return prevNode != nullptr; }

    const NodeT * getSuccessor() const { return nextNode; }
    const NodeT * getPredcessor() const { return prevNode; }
    NodeT * getSuccessor() { return nextNode; }
    NodeT * getPredcessor() { return prevNode; }

    unsigned int getDFSOrder() const
    {
        return analysisAuxData.dfsorder;
    }

#endif /* ENABLE_CFG */

    bool addSubgraph(DG *sub)
    {
        bool ret = subgraphs.insert(sub).second;

        if (ret) {
            // increase references of this graph
            // if we added it
            sub->ref();
        }

        return ret;
    }

    DGParameters<KeyT, NodeT> *
    addParameters(DGParameters<KeyT, NodeT> *params)
    {
        DGParameters<KeyT, NodeT> *old = parameters;

        parameters = params;
        return old;
    }

    const std::set<DG *>& getSubgraphs(void) const
    {
        return subgraphs;
    }

    bool hasSubgraphs() const
    {
        return !subgraphs.empty();
    }

    size_t subgraphsNum() const
    {
        return subgraphs.size();
    }

    DGParameters<KeyT, NodeT> *getParameters() const
    {
        return parameters;
    }

    KeyT getKey() const
    {
        return key;
    }

protected:

    // key uniquely identifying this node in a graph
    KeyT key;

private:
    // each node has reference to the DependenceGraph
    // it is in. Currently it is of type void *, because
    // of some casting issues
    void *dg;

#ifdef ENABLE_CFG
    // some analyses need classical CFG edges
    // and it is better to have even basic blocks
    BBlock<NodeT> *basicBlock;

    // successors of this node
    NodeT *nextNode;
    // predcessors of this node
    NodeT *prevNode;
#endif /* ENABLE_CFG */

    ControlEdgesT controlDepEdges;
    DependenceEdgesT dataDepEdges;

    // Nodes that have control/dep edge to this node
    ControlEdgesT revControlDepEdges;
    DependenceEdgesT revDataDepEdges;

    // a node can have more subgraphs (i. e. function pointers)
    std::set<DG *> subgraphs;

    // actual parameters if this is a callsite
    DGParameters<KeyT, NodeT> *parameters;

    // auxiliary data for different analyses
    analysis::AnalysesAuxiliaryData analysisAuxData;
    friend class analysis::Analysis<NodeT *>;
};

} // namespace dgg

#endif // _NODE_H_
