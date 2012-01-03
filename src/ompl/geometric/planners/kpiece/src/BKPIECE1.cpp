/*********************************************************************
* Software License Agreement (BSD License)
*
*  Copyright (c) 2011, Rice University,
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Rice University nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

/* Author: Ioan Sucan */

#include "ompl/geometric/planners/kpiece/BKPIECE1.h"
#include "ompl/base/GoalSampleableRegion.h"
#include "ompl/tools/config/SelfConfig.h"
#include <cassert>

ompl::geometric::BKPIECE1::BKPIECE1(const base::SpaceInformationPtr &si) : base::Planner(si, "BKPIECE1"),
                                                                           dStart_(boost::bind(&BKPIECE1::freeMotion, this, _1)),
                                                                           dGoal_(boost::bind(&BKPIECE1::freeMotion, this, _1))
{
    specs_.recognizedGoal = base::GOAL_SAMPLEABLE_REGION;

    minValidPathFraction_ = 0.5;
    failedExpansionScoreFactor_ = 0.5;
    maxDistance_ = 0.0;

    Planner::declareParam<double>("range", this, &BKPIECE1::setRange, &BKPIECE1::getRange);
    Planner::declareParam<double>("border_fraction", this, &BKPIECE1::setBorderFraction, &BKPIECE1::getBorderFraction);
    Planner::declareParam<double>("failed_expansion_score_factor", this, &BKPIECE1::setFailedExpansionCellScoreFactor, &BKPIECE1::getFailedExpansionCellScoreFactor);
    Planner::declareParam<double>("min_valid_path_fraction", this, &BKPIECE1::setMinValidPathFraction, &BKPIECE1::getMinValidPathFraction);
}

ompl::geometric::BKPIECE1::~BKPIECE1(void)
{
}

void ompl::geometric::BKPIECE1::setup(void)
{
    Planner::setup();
    SelfConfig sc(si_, getName());
    sc.configureProjectionEvaluator(projectionEvaluator_);
    sc.configurePlannerRange(maxDistance_);

    if (failedExpansionScoreFactor_ < std::numeric_limits<double>::epsilon() || failedExpansionScoreFactor_ > 1.0)
        throw Exception("Failed expansion cell score factor must be in the range (0,1]");
    if (minValidPathFraction_ < std::numeric_limits<double>::epsilon() || minValidPathFraction_ > 1.0)
        throw Exception("The minimum valid path fraction must be in the range (0,1]");

    dStart_.setDimension(projectionEvaluator_->getDimension());
    dGoal_.setDimension(projectionEvaluator_->getDimension());
}

bool ompl::geometric::BKPIECE1::solve(const base::PlannerTerminationCondition &ptc)
{
    checkValidity();
    base::GoalSampleableRegion *goal = dynamic_cast<base::GoalSampleableRegion*>(pdef_->getGoal().get());

    if (!goal)
    {
        msg_.error("Unknown type of goal (or goal undefined)");
        return false;
    }

    Discretization<Motion>::Coord xcoord;

    while (const base::State *st = pis_.nextStart())
    {
        Motion* motion = new Motion(si_);
        si_->copyState(motion->state, st);
        motion->root = motion->state;
        projectionEvaluator_->computeCoordinates(motion->state, xcoord);
        dStart_.addMotion(motion, xcoord);
    }

    if (dStart_.getMotionCount() == 0)
    {
        msg_.error("Motion planning start tree could not be initialized!");
        return false;
    }

    if (!goal->canSample())
    {
        msg_.error("Insufficient states in sampleable goal region");
        return false;
    }

    if (!sampler_)
        sampler_ = si_->allocValidStateSampler();

    msg_.inform("Starting with %d states", (int)(dStart_.getMotionCount() + dGoal_.getMotionCount()));

    std::vector<Motion*> solution;
    base::State *xstate = si_->allocState();
    bool      startTree = true;
    bool         solved = false;

    while (ptc() == false)
    {
        Discretization<Motion> &disc      = startTree ? dStart_ : dGoal_;
        startTree = !startTree;
        Discretization<Motion> &otherDisc = startTree ? dStart_ : dGoal_;
        disc.countIteration();

        // if we have not sampled too many goals already
        if (dGoal_.getMotionCount() == 0 || pis_.getSampledGoalsCount() < dGoal_.getMotionCount() / 2)
        {
            const base::State *st = dGoal_.getMotionCount() == 0 ? pis_.nextGoal(ptc) : pis_.nextGoal();
            if (st)
            {
                Motion* motion = new Motion(si_);
                si_->copyState(motion->state, st);
                motion->root = motion->state;
                projectionEvaluator_->computeCoordinates(motion->state, xcoord);
                dGoal_.addMotion(motion, xcoord);
            }
            if (dGoal_.getMotionCount() == 0)
            {
                msg_.error("Unable to sample any valid states for goal tree");
                break;
            }
        }

        Discretization<Motion>::Cell *ecell    = NULL;
        Motion                       *existing = NULL;
        disc.selectMotion(existing, ecell);
        assert(existing);
        if (sampler_->sampleNear(xstate, existing->state, maxDistance_))
        {
            std::pair<base::State*, double> fail(xstate, 0.0);
            bool keep = si_->checkMotion(existing->state, xstate, fail);
            if (!keep && fail.second > minValidPathFraction_)
                keep = true;

            if (keep)
            {
                /* create a motion */
                Motion *motion = new Motion(si_);
                si_->copyState(motion->state, xstate);
                motion->root = existing->root;
                motion->parent = existing;

                projectionEvaluator_->computeCoordinates(motion->state, xcoord);
                disc.addMotion(motion, xcoord);

                Discretization<Motion>::Cell* cellC = otherDisc.getGrid().getCell(xcoord);

                if (cellC && !cellC->data->motions.empty())
                {
                    Motion* connectOther = cellC->data->motions[rng_.uniformInt(0, cellC->data->motions.size() - 1)];

                    if (goal->isStartGoalPairValid(startTree ? connectOther->root : motion->root, startTree ? motion->root : connectOther->root) &&
                        si_->checkMotion(motion->state, connectOther->state))
                    {
                        /* extract the motions and put them in solution vector */

                        std::vector<Motion*> mpath1;
                        while (motion != NULL)
                        {
                            mpath1.push_back(motion);
                            motion = motion->parent;
                        }

                        std::vector<Motion*> mpath2;
                        while (connectOther != NULL)
                        {
                            mpath2.push_back(connectOther);
                            connectOther = connectOther->parent;
                        }

                        if (startTree)
                            mpath1.swap(mpath2);

                        PathGeometric *path = new PathGeometric(si_);
                        path->states.reserve(mpath1.size() + mpath2.size());
                        for (int i = mpath1.size() - 1 ; i >= 0 ; --i)
                            path->states.push_back(si_->cloneState(mpath1[i]->state));
                        for (unsigned int i = 0 ; i < mpath2.size() ; ++i)
                            path->states.push_back(si_->cloneState(mpath2[i]->state));

                        goal->addSolutionPath(base::PathPtr(path), false, 0.0);
                        solved = true;
                        break;
                    }
                }
            }
            else
            {
                ecell->data->score *= failedExpansionScoreFactor_;
                disc.updateCell(ecell);
            }
        }
        else
            disc.updateCell(ecell);
    }

    si_->freeState(xstate);

    msg_.inform("Created %u (%u start + %u goal) states in %u cells (%u start (%u on boundary) + %u goal (%u on boundary))",
                dStart_.getMotionCount() + dGoal_.getMotionCount(), dStart_.getMotionCount(), dGoal_.getMotionCount(),
                dStart_.getCellCount() + dGoal_.getCellCount(), dStart_.getCellCount(), dStart_.getGrid().countExternal(),
                dGoal_.getCellCount(), dGoal_.getGrid().countExternal());

    return solved;
}

void ompl::geometric::BKPIECE1::freeMotion(Motion *motion)
{
    if (motion->state)
        si_->freeState(motion->state);
    delete motion;
}

void ompl::geometric::BKPIECE1::clear(void)
{
    Planner::clear();

    sampler_.reset();
    dStart_.clear();
    dGoal_.clear();
}

void ompl::geometric::BKPIECE1::getPlannerData(base::PlannerData &data) const
{
    Planner::getPlannerData(data);
    dStart_.getPlannerData(data, 1);
    dGoal_.getPlannerData(data, 2);
}
