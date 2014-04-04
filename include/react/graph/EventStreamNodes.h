
//          Copyright Sebastian Jeckel 2014.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include "react/Defs.h"

#include <atomic>
#include <algorithm>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "tbb/spin_mutex.h"

#include "GraphBase.h"
#include "react/common/Types.h"


/***************************************/ REACT_IMPL_BEGIN /**************************************/

///////////////////////////////////////////////////////////////////////////////////////////////////
/// EventStreamNode
///////////////////////////////////////////////////////////////////////////////////////////////////
template
<
    typename D,
    typename E
>
class EventStreamNode : public ReactiveNode<D,E,void>
{
public:
    using EventListT    = std::vector<E>;
    using EventMutexT   = tbb::spin_mutex;

    using PtrT      = std::shared_ptr<EventStreamNode>;
    using WeakPtrT  = std::weak_ptr<EventStreamNode>;

    using EngineT   = typename D::Engine;
    using TurnT     = typename EngineT::TurnT;

    EventStreamNode() :
        ReactiveNode()
    {
    }

    virtual const char* GetNodeType() const override    { return "EventStreamNode"; }

    void SetCurrentTurn(const TurnT& turn, bool forceUpdate = false, bool noClear = false)
    {// eventMutex_
        EventMutexT::scoped_lock lock(eventMutex_);

        if (curTurnId_ != turn.Id() || forceUpdate)
        {
            curTurnId_ =  turn.Id();
            if (!noClear)
                events_.clear();
        }
    }// ~eventMutex_

    void ClearEvents(const TurnT& turn)
    {
        curTurnId_ =  turn.Id();
        events_.clear();
    }

    EventListT&     Events()        { return events_; }

protected:
    EventListT    events_;
    EventMutexT   eventMutex_;

    uint    curTurnId_ = INT_MAX;
};

template <typename D, typename E>
using EventStreamNodePtr = typename EventStreamNode<D,E>::PtrT;

template <typename D, typename E>
using EventStreamNodeWeakPtr = typename EventStreamNode<D,E>::WeakPtrT;

///////////////////////////////////////////////////////////////////////////////////////////////////
/// EventSourceNode
///////////////////////////////////////////////////////////////////////////////////////////////////
template
<
    typename D,
    typename E
>
class EventSourceNode :
    public EventStreamNode<D,E>,
    public IInputNode
{
public:
    EventSourceNode() :
        EventStreamNode()
    {
        Engine::OnNodeCreate(*this);
    }

    ~EventSourceNode()
    {
        Engine::OnNodeDestroy(*this);
    }

    virtual const char* GetNodeType() const override    { return "EventSourceNode"; }

    virtual void Tick(void* turnPtr) override
    {
        REACT_ASSERT(false, "Don't tick the EventSourceNode\n");
        return;
    }

    virtual bool IsInputNode() const override    { return true; }

    template <typename V>
    void AddInput(V&& v)
    {
        // Clear input from previous turn
        if (changedFlag_)
        {
            changedFlag_ = false;
            events_.clear();
        }

        events_.push_back(std::forward<V>(v));
    }

    virtual bool ApplyInput(void* turnPtr) override
    {
        if (events_.size() > 0 && !changedFlag_)
        {
            using TurnT = typename D::Engine::TurnT;
            TurnT& turn = *static_cast<TurnT*>(turnPtr);

            SetCurrentTurn(turn, true, true);
            changedFlag_ = true;
            Engine::OnTurnInputChange(*this, turn);
            return true;
        }
        else
        {
            return false;
        }
    }

private:
    bool changedFlag_ = false;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
/// EventMergeOp
///////////////////////////////////////////////////////////////////////////////////////////////////
template
<
    typename E,
    typename ... TDeps
>
class EventMergeOp : public ReactiveOpBase<TDeps...>
{
public:
    template <typename ... TDepsIn>
    EventMergeOp(TDepsIn&& ... deps) :
        ReactiveOpBase(0u, std::forward<TDepsIn>(deps) ...)
    {}

    EventMergeOp(EventMergeOp&& other) :
        ReactiveOpBase{ std::move(other) }
    {}

    template <typename TTurn, typename TCollector>
    void Collect(const TTurn& turn, const TCollector& collector) const
    {
        apply(CollectFunctor<TTurn, TCollector>{ turn, collector }, deps_);
    }

    template <typename TTurn, typename TCollector, typename TFunctor>
    void CollectRec(const TFunctor& functor) const
    {
        apply(reinterpret_cast<const CollectFunctor<TTurn,TCollector>&>(functor), deps_);
    }

private:
    template <typename TTurn, typename TCollector>
    struct CollectFunctor
    {
        CollectFunctor(const TTurn& turn, const TCollector& collector) :
            MyTurn{ turn },
            MyCollector{ collector }
        {}

        void operator()(const TDeps& ... deps) const
        {
            REACT_EXPAND_PACK(collect(deps));
        }

        template <typename T>
        void collect(const T& op) const
        {
            op.CollectRec<TTurn,TCollector>(*this);
        }

        template <typename T>
        void collect(const NodeHolderT<T>& depPtr) const
        {
            depPtr->SetCurrentTurn(MyTurn);

            for (const auto& v : depPtr->Events())
                MyCollector(v);
        }

        const TTurn&        MyTurn;
        const TCollector&   MyCollector;
    };
};

///////////////////////////////////////////////////////////////////////////////////////////////////
/// EventFilterOp
///////////////////////////////////////////////////////////////////////////////////////////////////
template
<
    typename E,
    typename TFilter,
    typename TDep
>
class EventFilterOp : public ReactiveOpBase<TDep>
{
public:
    template <typename TFilterIn, typename TDepIn>
    EventFilterOp(TFilterIn&& filter, TDepIn&& dep) :
        ReactiveOpBase(0u, std::forward<TDepIn>(dep)),
        filter_{ std::forward<TFilterIn>(filter) }
    {}

    EventFilterOp(EventFilterOp&& other) :
        ReactiveOpBase{ std::move(other) },
        filter_{ std::move(other.filter_) }
    {}

    template <typename TTurn, typename TCollector>
    void Collect(const TTurn& turn, const TCollector& collector) const
    {
        collectImpl(turn, FilteredEventCollector<TCollector>{ filter_, collector }, getDep());
    }

    template <typename TTurn, typename TCollector, typename TFunctor>
    void CollectRec(const TFunctor& functor) const
    {
        // Can't recycle functor because MyFunc needs replacing
        Collect<TTurn,TCollector>(functor.MyTurn, functor.MyCollector);
    }

private:
    const TDep& getDep() const { return std::get<0>(deps_); }

    template <typename TFiltered>
    struct FilteredEventCollector
    {
        FilteredEventCollector(const TFilter& filter, const TFiltered& filtered) :
            MyFilter{ filter },
            MyFiltered{ filtered }
        {}

        void operator()(const E& e) const
        {
            // Accepted?
            if (MyFilter(e))
                MyFiltered(e);
        }

        const TFilter&      MyFilter;
        const TFiltered&    MyFiltered;
    };

    template <typename TTurn, typename TCollector, typename T>
    static void collectImpl(const TTurn& turn, const TCollector& collector, const T& op)
    {
       op.Collect(turn, collector);
    }

    template <typename TTurn, typename TCollector, typename T>
    static void collectImpl(const TTurn& turn, const TCollector& collector, const NodeHolderT<T>& depPtr)
    {
        depPtr->SetCurrentTurn(turn);

        for (const auto& v : depPtr->Events())
            collector(v);
    }

    TFilter filter_;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
/// EventTransformOp
///////////////////////////////////////////////////////////////////////////////////////////////////
// Todo: Refactor code duplication
template
<
    typename E,
    typename TFunc,
    typename TDep
>
class EventTransformOp : public ReactiveOpBase<TDep>
{
public:
    template <typename TFuncIn, typename TDepIn>
    EventTransformOp(TFuncIn&& func, TDepIn&& dep) :
        ReactiveOpBase(0u, std::forward<TDepIn>(dep)),
        func_{ std::forward<TFuncIn>(func) }
    {}

    EventTransformOp(EventTransformOp&& other) :
        ReactiveOpBase{ std::move(other) },
        func_{ std::move(other.func_) }
    {}

    template <typename TTurn, typename TCollector>
    void Collect(const TTurn& turn, const TCollector& collector) const
    {
        collectImpl(turn, TransformEventCollector<TCollector>{ func_, collector }, getDep());
    }

    template <typename TTurn, typename TCollector, typename TFunctor>
    void CollectRec(const TFunctor& functor) const
    {
        // Can't recycle functor because MyFunc needs replacing
        Collect<TTurn,TCollector>(functor.MyTurn, functor.MyCollector);
    }

private:
    const TDep& getDep() const { return std::get<0>(deps_); }

    template <typename TTarget>
    struct TransformEventCollector
    {
        TransformEventCollector(const TFunc& func, const TTarget& target) :
            MyFunc{ func },
            MyTarget{ target }
        {}

        void operator()(const E& e) const
        {
            MyTarget(MyFunc(e));
        }

        const TFunc&    MyFunc;
        const TTarget&  MyTarget;
    };

    template <typename TTurn, typename TCollector, typename T>
    static void collectImpl(const TTurn& turn, const TCollector& collector, const T& op)
    {
       op.Collect(turn, collector);
    }

    template <typename TTurn, typename TCollector, typename T>
    static void collectImpl(const TTurn& turn, const TCollector& collector, const NodeHolderT<T>& depPtr)
    {
        depPtr->SetCurrentTurn(turn);

        for (const auto& v : depPtr->Events())
            collector(v);
    }

    TFunc func_;
};

///////////////////////////////////////////////////////////////////////////////////////////////////
/// EventOpNode
///////////////////////////////////////////////////////////////////////////////////////////////////
template
<
    typename D,
    typename E,
    typename TOp
>
class EventOpNode : public EventStreamNode<D,E>
{
public:
    template <typename ... TArgs>
    EventOpNode(TArgs&& ... args) :
        EventStreamNode<D,E>(),
        op_{ std::forward<TArgs>(args) ... }
    {
        Engine::OnNodeCreate(*this);
        op_.Attach<D>(*this);
    }

    ~EventOpNode()
    {
        if (!wasOpStolen_)
            op_.Detach<D>(*this);
        Engine::OnNodeDestroy(*this);
    }

    virtual const char* GetNodeType() const override    { return "EventOpNode"; }

    virtual void Tick(void* turnPtr) override
    {
        using TurnT = typename D::Engine::TurnT;
        TurnT& turn = *static_cast<TurnT*>(turnPtr);

        SetCurrentTurn(turn, true);

        REACT_LOG(D::Log().template Append<NodeEvaluateBeginEvent>(
            GetObjectId(*this), turn.Id()));

        op_.Collect(turn, EventCollector{ Events() });

        REACT_LOG(D::Log().template Append<NodeEvaluateEndEvent>(
            GetObjectId(*this), turn.Id()));

        if (! events_.empty())
            Engine::OnNodePulse(*this, turn);
        else
            Engine::OnNodeIdlePulse(*this, turn);
    }

    virtual int DependencyCount() const override
    {
        return TOp::dependency_count;
    }

    TOp StealOp()
    {
        REACT_ASSERT(wasOpStolen_ == false, "Op was already stolen.");
        wasOpStolen_ = true;
        op_.Detach<D>(*this);
        return std::move(op_);
    }

private:
    struct EventCollector
    {
        EventCollector(EventListT& events) : MyEvents{ events }
        {}

        void operator()(const E& e) const { MyEvents.push_back(e); }

        EventListT& MyEvents;
    };

    TOp     op_;
    bool    wasOpStolen_ = false;
};

/****************************************/ REACT_IMPL_END /***************************************/
