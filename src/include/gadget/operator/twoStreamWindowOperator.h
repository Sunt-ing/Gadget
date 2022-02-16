//
// Created by Showan Asyabi on 5/23/21.
//

#ifndef GADGET_TWOSTREAMWINDOWOPERATOR_H
#define GADGET_TWOSTREAMWINDOWOPERATOR_H

#include <memory>
#include <string>
#include <vector>
#include <list>
#include <iostream>
#include <map>
#include <unordered_map>
#include <climits>

#include "include/gadget/distributions/arrival.h"
#include "include/gadget/distributions/key.h"
#include "include/gadget/distributions/value.h"
#include "include/gadget/distributions/windowLength.h"
#include "include/gadget/distributions/serviceTime.h"
#include "include/gadget/operation.h"
#include "include/gadget/statemachine/stateMachines.h"
#include "include/gadget/event/event.h"
#include "include/gadget/statemachine/stateMachineBuilder.h"
#include "gadget/event/eventGenerator.h"
#include "gadget/operator/operator.h"
#include "gadget/operator/operators/operatorParameters.h"


class TwoStreamWindowOperator :  public Operator {

public:
#if 0
    TwoStreamWindowOperator(std::shared_ptr<OperatorParameters> params) {
        keyedOperator_ = params->keyedOperator_ ;
        windowLength_ = params->windowLength;
        latenessAllowed_ = params->latenessLength;
        latenessLength_ =  params->latenessLength;
        keyDistribution_ = params->keysDistribution;
        valueDistribution_ = params->valuesDistribution;
        eventGenerator_ = params->eventGenerator;
        eventGenerator2_ = params->eventGenerator2;
        serviceTimeDistribution_ = params->serviceTimeDistribution;
        stateMachineType_ = params->stateMachineType;
    }
#endif
    /// get all tne needed windows
    virtual void  assignWindows(const std::shared_ptr<Event> &event, std::vector<uint64_t> &windowStartTimes) = 0;


    bool runOperator(std::vector<std::shared_ptr<Operation>> &operationList) {

        //  get the events of the next unit of time from the event generator
        std::vector<std::shared_ptr<Event>> leftStreamEventBatch;
        std::vector<std::shared_ptr<Event>> rightStreamEventBatch;
        leftStreamEventBatch.clear();
        rightStreamEventBatch.clear();
        uint64_t  leftStreamTime = eventGenerator_->getEventBatch(leftStreamEventBatch);
        uint64_t  rightStreamTime = eventGenerator2_->getEventBatch(rightStreamEventBatch);

        if(leftStreamTime == 0 &&  rightStreamTime == 0)  { // this indicates there are no more events to process
            return false;
        }

        uint64_t leftStreamWaterMark= (leftStreamTime)? eventGenerator_->getWaterMark(): LLONG_MAX;
        uint64_t rightStreamWaterMark= (rightStreamTime)? eventGenerator_->getWaterMark(): LLONG_MAX;
        uint64_t newWaterMark;
        eventGenerator_->forgetPanes(leftStreamTime);
        eventGenerator2_->forgetPanes(rightStreamTime);

        // 2 make windows the new event batch
        if(leftStreamTime) {
            makeWindows(leftStreamEventBatch);
        }
        if(rightStreamTime) {
            makeWindows(rightStreamEventBatch);
        }

        if(leftStreamTime) {
            generateOperations(leftStreamEventBatch, operationList, true);
        }
        if(rightStreamTime) {
            generateOperations(rightStreamEventBatch, operationList, false);
        }
        newWaterMark = std::min(leftStreamWaterMark, rightStreamWaterMark);

        for (int time = waterMark_ + 1  ; time <= newWaterMark; time++) {
            expireStateMachines(time, operationList);
        }
        // update the watermark
        waterMark_ = newWaterMark;
        return true;
    }


    /**
    *  get all needed state machines (windows) for current event batch
    * @param eventBatch
    * @param stateMachineList
    */
    void  generateOperations(std::vector<std::shared_ptr<Event>> &eventBatch, std::vector<std::shared_ptr<Operation>> &operationList, bool leftStream) {
        for(const auto &event :eventBatch) {
            std::string eventKey = (keyedOperator_)? event->key_ : "allKeys";
            for(auto &m : stateMachineDictionary_[eventKey]) {
                if (event->eventTime_ >= m->getStartTime() && event->eventTime_ < m->getFinishTime()) {
                    // reset the state machine for the current event
                    m->reset(event, leftStream);
                    while (m->HasNext()) {
                        // make a new operation
                        auto opr = std::make_shared<Operation>();
                        opr->oprType = m->NextOperation();
                        opr->key = m->getMachineKey();
                        opr->value = valueDistribution_->Next();
                        //opr->sleepTime = serviceTimeDistribution_->Next(); // fixme
                        operationList.push_back(opr);
                    }
                }
            }
        }
    }
    void makeWindows(std::vector<std::shared_ptr<Event>> eventBatch) {
        for(const auto &event :eventBatch) {
            std::vector<uint64_t> neededWindows; // this vector holds the start time of needed windows
            neededWindows.clear();
            assignWindows(event, neededWindows);
            std::sort(neededWindows.begin(), neededWindows.end());
            std::string eventKey = (keyedOperator_)? event->key_ : "allKeys";
            // for each window if it does not exist, make it and push it
            for (auto const &wStartTime :neededWindows) {
                uint64_t windowFinishTime = wStartTime + windowLength_;
                if(stateMachineDictionaryTimeProgress_.find(eventKey) != stateMachineDictionaryTimeProgress_.end()) {
                    if (stateMachineDictionaryTimeProgress_[eventKey] >= windowFinishTime) {
                        continue;
                    }
                }

                // windows are sorted.  this  means the  needed window does not exist
                if (stateMachineDictionary_.find(eventKey) == stateMachineDictionary_.end() ||  stateMachineDictionary_[eventKey].back()->getFinishTime() < windowFinishTime) {
                    auto newStateMachines = StateMachineBuilder::BuildStateMachine(stateMachineType_, keyDistribution_->Next()
                            , wStartTime, windowFinishTime);
                    if(!newStateMachines.has_value()) {
                        throw std::exception();
                    }
                    stateMachineDictionary_[eventKey].push_back(newStateMachines.value());
                    stateMachineDictionaryTimeProgress_[eventKey] = windowFinishTime;
                    if (latenessAllowed_) {
                        expiringStateMachinesDictionary_[windowFinishTime + latenessLength_].push_back(newStateMachines.value());
                    } else {
                        expiringStateMachinesDictionary_[windowFinishTime].push_back(newStateMachines.value());
                    }
                }
            }
        }
    }
/***
 * this function searches the current state machines (windows) that can be closed at this time
 * The function returns all state machines tat can be closed at this time
 * @return
 */
    void  expireStateMachines(uint64_t time, std::vector<std::shared_ptr<Operation>> &operationList) {
        if (expiringStateMachinesDictionary_.find(time) !=  expiringStateMachinesDictionary_.end()) {

            for(const auto &m : expiringStateMachinesDictionary_[time]) {
                m->closeStateMachine();
                while(m->HasNext()) {
                    // make a new operation
                    auto opr =  std::make_shared<Operation>();
                    opr->oprType = m->NextOperation();
                    opr->key = m->getMachineKey();
                    opr->value = valueDistribution_->Next();
                    //opr->sleepTime = serviceTimeDistribution_->Next(); // fixme
                    operationList.push_back(opr);
                }
            }

            // remove these machines from the  window dictionary
            for(const auto &m : expiringStateMachinesDictionary_[time]) {
                std::string Key = (keyedOperator_)? m->getUserKey() : "allKeys";
                auto &windows = stateMachineDictionary_[Key];
                auto itr = windows.begin();
                while (itr != windows.end()) {
                    if (m->getStartTime() == (*itr)->getStartTime() && m->getFinishTime() == (*itr)->getFinishTime()) {
                        itr = windows.erase(itr);
                    } else {
                        ++itr;
                    }
                }
                if(windows.size() == 0) {
                    stateMachineDictionary_.erase(Key) ;
                }
            }
            expiringStateMachinesDictionary_.erase(time);
        }
    }


protected:

    bool keyedOperator_;
    /***
     * the distribution keys
     */
    std::shared_ptr<Key>     keyDistribution_;
    /**
     * the distribution of values
     */
    std::shared_ptr<Value>   valueDistribution_;


    /**
     * The event generator that generates events
     */
    std::shared_ptr<EventGenerator> eventGenerator_;

    /**
     * The second event generator for two streams operators(e.g., join)
     */
    std::shared_ptr<EventGenerator> eventGenerator2_;


    /***
     * current waterMark
     */
    uint64_t  waterMark_;

    /***
    * current gadget time
    */
    uint64_t  timeUnit_;


    /***
     * windowLength_ is the length  of the window in terms of time units
     */
    uint64_t  windowLength_;

    /***
     * slidingLength_ is the length  of the sliding in terms of time units
     */
    uint64_t  slidingLength_;

    /***
     *  latenessLength_ is the extra amount of time that a window should wait before being closed
     */
    uint64_t  latenessLength_;

    /***
     * indicates   if the system processes  out of order events
     */
    bool latenessAllowed_;

    /***
     * the service time distribution (service rate)
     */
    std::shared_ptr<ServiceTime> serviceTimeDistribution_;

    /***
     *  We keep track of active state machines using an unordered map - key is the user key and
     *  value is the list of stata machines that are active for the user key
     *  Please note that terms stateMachine and window is used interchangeably
     */
    std::unordered_map<std::string ,std::list<std::shared_ptr<StateMachines>>> stateMachineDictionary_;


    /***
     *  we keep track of to-be-expired state machines using an unordered map - key is a finish time and the value is
     *  the list of state machine that must be closed at that finish time
     */
    std::unordered_map< uint64_t ,std::list<std::shared_ptr<StateMachines>>> expiringStateMachinesDictionary_;


    /***
     * this dictionary tracks the time progress for each key. I use this dictionary to prevent out of order events
     * to make a new windows if  that window has been already evaluated
     */
    std::unordered_map<std::string, uint64_t> stateMachineDictionaryTimeProgress_;


    StateMachines::StateMachineType stateMachineType_;


    uint64_t gadgetTime_;
};

#endif //GADGET_TWOSTREAMWINDOWOPERATOR_H
