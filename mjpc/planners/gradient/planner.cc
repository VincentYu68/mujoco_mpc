// Copyright 2022 DeepMind Technologies Limited
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "planners/gradient/planner.h"

#include <algorithm>
#include <chrono>
#include <mutex>

#include "array_safety.h"
#include "planners/cost_derivatives.h"
#include "planners/gradient/gradient.h"
#include "planners/gradient/policy.h"
#include "planners/gradient/settings.h"
#include "planners/model_derivatives.h"
#include "states/state.h"
#include "trajectory.h"
#include "utilities.h"

namespace mjpc {
namespace mju = ::mujoco::util_mjpc;

// initialize planner settings
void GradientPlanner::Initialize(mjModel* model, const Task& task) {
  // delete mjData instances since model might have changed.
  data_.clear();
  // allocate one mjData for nominal.
  ResizeMjData(model, 1);

  // model
  this->model = model;

  // task
  this->task = &task;

  // rollout parameters
  timestep_power = 1.0;

  // dimensions
  dim_state = model->nq + model->nv + model->na;  // state dimension
  dim_state_derivative =
      2 * model->nv + model->na;    // state derivative dimension
  dim_action = model->nu;           // action dimension
  dim_sensor = model->nsensordata;  // number of sensor values
  dim_max = 10 * mju_max(mju_max(mju_max(dim_state, dim_state_derivative),
                                 dim_action),
                         model->nuser_sensor);
  num_trajectory = GetNumberOrDefault(32, model, "gradient_num_trajectory");
}

// allocate memory
void GradientPlanner::Allocate() {
  // state
  state.resize(model->nq + model->nv + model->na);
  mocap.resize(7 * model->nmocap);

  // candidate trajectories
  for (int i = 0; i < kMaxTrajectory; i++) {
    trajectory[i].Initialize(dim_state, dim_action, task->num_residual,
                             kMaxTrajectoryHorizon);
    trajectory[i].Allocate(kMaxTrajectoryHorizon);
  }

  // model derivatives
  model_derivative.Allocate(dim_state_derivative, dim_action, dim_sensor,
                            kMaxTrajectoryHorizon);

  // costs derivatives
  cost_derivative.Allocate(dim_state_derivative, dim_action, task->num_residual,
                           kMaxTrajectoryHorizon, dim_max);

  // gradient descent
  gradient.Allocate(dim_state_derivative, dim_action, kMaxTrajectoryHorizon);

  // spline mapping
  for (auto& mapping : mappings) {
    mapping->Allocate(model->nu);
  }

  // policy
  for (int i = 0; i < kMaxTrajectory; i++) {
    candidate_policy[i].Allocate(model, *task, kMaxTrajectoryHorizon);
  }
  policy.Allocate(model, *task, kMaxTrajectoryHorizon);

  // scratch
  parameters_scratch.resize(model->nu * kMaxTrajectoryHorizon);
  times_scratch.resize(kMaxTrajectoryHorizon);
}

// reset memory to zeros
void GradientPlanner::Reset(int horizon) {
  // state
  std::fill(state.begin(), state.end(), 0.0);
  std::fill(mocap.begin(), mocap.end(), 0.0);
  time = 0.0;

  // model derivatives
  model_derivative.Reset(dim_state_derivative, dim_action, dim_sensor, horizon);

  // cost derivatives
  cost_derivative.Reset(dim_state_derivative, dim_action, task->num_residual,
                        horizon);

  // gradient
  gradient.Reset(dim_state_derivative, dim_action, horizon);

  // policy
  for (int i = 0; i < kMaxTrajectory; i++) {
    candidate_policy[i].Reset(horizon);
  }
  policy.Reset(horizon);

  // scratch
  std::fill(parameters_scratch.begin(), parameters_scratch.end(), 0.0);
  std::fill(times_scratch.begin(), times_scratch.end(), 0.0);

  // candidate trajectories
  for (int i = 0; i < kMaxTrajectory; i++) {
    trajectory[i].Reset(horizon);
  }

  // values
  step_size = 0.0;
  expected = 0.0;
  improvement = 0.0;
  surprise = 0.0;
}

// set state
void GradientPlanner::SetState(State& state) {
  state.CopyTo(this->state.data(), this->mocap.data(), &this->time);
}

// optimize nominal policy via gradient descent
void GradientPlanner::OptimizePolicy(int horizon, ThreadPool& pool) {
  ResizeMjData(model, pool.NumThreads());
  // timers
  double nominal_time = 0.0;
  double model_derivative_time = 0.0;
  double cost_derivative_time = 0.0;
  double rollouts_time = 0.0;
  double gradient_time = 0.0;
  double policy_update_time = 0.0;

  // maximum number of trajectories in linesearch
  num_trajectory = mju_min(num_trajectory, kMaxTrajectory);

  // ---- nominal rollout ----- //
  // start timer
  auto nominal_start = std::chrono::steady_clock::now();

  // copy nominal policy
  policy.num_parameters = model->nu * policy.num_spline_points;
  {
    const std::shared_lock<std::shared_mutex> lock(mtx_);
    candidate_policy[0].CopyFrom(policy, policy.num_spline_points);
  }

  // resample policy
  this->ResamplePolicy(horizon);

  // rollout nominal trajectory
  this->NominalTrajectory(horizon);

  // previous best cost
  double c_prev = trajectory[0].total_return;

  // stop timer
  nominal_time = std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - nominal_start)
                     .count();

  // update policy
  double c_best = c_prev;
  for (int i = 0; i < settings.max_rollout; i++) {
    // ----- model derivatives ----- //
    // start timer
    auto model_derivative_start = std::chrono::steady_clock::now();

    // compute model and sensor Jacobians
    model_derivative.Compute(
        model, data_, trajectory[0].states.data(),
        trajectory[0].actions.data(), dim_state,
        dim_state_derivative, dim_action, dim_sensor, horizon,
        settings.fd_tolerance, settings.fd_mode, pool);

    // stop timer
    model_derivative_time +=
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - model_derivative_start)
            .count();

    // -----cost derivatives ----- //
    // start timer
    auto cost_derivative_start = std::chrono::steady_clock::now();

    // compute cost derivatives
    cost_derivative.Compute(
        trajectory[0].residual.data(),
        model_derivative.C.data(), model_derivative.D.data(),
        dim_state_derivative, dim_action, dim_max, dim_sensor,
        task->num_residual, task->dim_norm_residual.data(), task->num_norms,
        task->weight.data(), task->norm.data(), task->norm_parameters.data(),
        task->num_norm_parameters.data(), task->risk, horizon, pool);

    // stop timer
    cost_derivative_time +=
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - cost_derivative_start)
            .count();

    // ----- gradient descent ----- //
    // start timer
    auto gradient_start = std::chrono::steady_clock::now();

    // compute action derivatives
    int gd_status = gradient.Compute(&candidate_policy[0], &model_derivative,
                                     &cost_derivative, dim_state_derivative,
                                     dim_action, horizon);

    // compute spline mapping derivatives
    mappings[policy.representation]->Compute(
        candidate_policy[0].times.data(), candidate_policy[0].parameters.data(),
        candidate_policy[0].num_spline_points,
        trajectory[0].times.data(),
        trajectory[0].horizon - 1);

    // compute total derivatives
    mju_mulMatTVec(candidate_policy[0].parameter_update.data(),
                   mappings[policy.representation]->Get(),
                   candidate_policy[0].k.data(),
                   model->nu * (trajectory[0].horizon - 1),
                   model->nu * candidate_policy[0].num_spline_points);

    // stop timer
    gradient_time += std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - gradient_start)
                         .count();

    // check for failure
    if (gd_status != 0) return;

    // TODO(taylorhowell): normalize gradient heuristic for cost scale invariance
    // double grad_norm =
    //     mju_norm(candidate_policy[0].parameter_update.data(),
    //              model->nu * candidate_policy[0].num_spline_points);
    // mju_scl(candidate_policy[0].parameter_update.data(),
    //         candidate_policy[0].parameter_update.data(),
    //         1.0 / grad_norm / candidate_policy[0].num_spline_points,
    //         model->nu * candidate_policy[0].num_spline_points);

    // ----- rollout policy ----- //
    // start timer
    auto rollouts_start = std::chrono::steady_clock::now();

    // copy policy
    for (int i = 1; i < num_trajectory; i++) {
      candidate_policy[i].CopyFrom(candidate_policy[0], candidate_policy[0].num_spline_points);
    }

    // improvement step sizes
    LogScale(improvement_step, 1.0, settings.min_step_size,
              num_trajectory - 1);
    improvement_step[num_trajectory - 1] = 0.0;

    // rollouts (parallel)
    this->Rollouts(horizon, pool);

    // ----- evaluate rollouts ------ //
    winner = num_trajectory - 1;
    for (int j = num_trajectory - 1; j >= 0; j--) {
      // compute cost
      double c_sample = trajectory[j].total_return;

      // compare cost
      if (c_sample < c_best) {
        c_best = c_sample;
        winner = j;
      }
    }

    // update nominal with winner
    candidate_policy[0].CopyParametersFrom(candidate_policy[winner].parameters, candidate_policy[winner].times);
    trajectory[0] = trajectory[winner];

    // improvement
    step_size = improvement_step[winner];
    expected = -step_size * (gradient.dV[0]) - 1.0e-16;
    improvement = c_prev - c_best;
    surprise = mju_min(mju_max(0, improvement / expected), 2);

    // stop timer
    rollouts_time += std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - rollouts_start)
                         .count();
  }


  // update nominal policy
  auto policy_update_start = std::chrono::steady_clock::now();

  // check for improvement
  if (c_best >= c_prev) {
    winner = num_trajectory - 1;
  }

  {
    const std::shared_lock<std::shared_mutex> lock(mtx_);
    policy.CopyParametersFrom(candidate_policy[winner].parameters, candidate_policy[winner].times);
  }

  // stop timer
  policy_update_time +=
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - policy_update_start)
          .count();

  // set timers
  nominal_compute_time = nominal_time;
  model_derivative_compute_time = model_derivative_time;
  cost_derivative_compute_time = cost_derivative_time;
  rollouts_compute_time = rollouts_time;
  gradient_compute_time = gradient_time;
  policy_update_compute_time = policy_update_time;
}

// compute trajectory using nominal policy
void GradientPlanner::NominalTrajectory(int horizon) {
  // nominal policy
  auto nominal_policy = [&cp = candidate_policy[0]](double* action, const double* state,
                                           double time) {
    cp.Action(action, state, time);
  };

  // nominal policy rollout
  trajectory[0].Rollout(nominal_policy, task, model, data_[0].get(), state.data(),
                        time, mocap.data(), horizon);
}

// compute action from policy
void GradientPlanner::ActionFromPolicy(double* action, const double* state,
                                       double time) {
  const std::shared_lock<std::shared_mutex> lock(mtx_);
  policy.Action(action, state, time);
}

// update policy for current time
void GradientPlanner::ResamplePolicy(int horizon) {
  // dimensions
  int num_parameters = candidate_policy[0].num_parameters;
  int num_spline_points = candidate_policy[0].num_spline_points;

  // time
  double nominal_time = time;
  double time_shift = mju_max(
      (horizon - 1) * model->opt.timestep / (num_spline_points - 1),
      1.0e-5);

  // get spline points
  for (int t = 0; t < num_spline_points; t++) {
    times_scratch[t] = nominal_time;
    candidate_policy[0].Action(DataAt(parameters_scratch, t * model->nu), nullptr,
                  nominal_time);
    nominal_time += time_shift;
  }

  // copy resampled policy parameters
  mju_copy(candidate_policy[0].parameters.data(), parameters_scratch.data(),
           num_parameters);
  mju_copy(candidate_policy[0].times.data(), times_scratch.data(),
           num_spline_points);

  // time step power scaling
  PowerSequence(candidate_policy[0].times.data(), time_shift, candidate_policy[0].times[0],
                candidate_policy[0].times[num_spline_points - 1], timestep_power,
                num_spline_points);
}

// compute candidate trajectories
void GradientPlanner::Rollouts(int horizon, ThreadPool& pool) {
  int count_before = pool.GetCount();
  for (int i = 0; i < num_trajectory; i++) {
    pool.Schedule([&data = data_, &trajectory = trajectory,
                    &candidate_policy = candidate_policy,
                    &improvement_step = improvement_step, &model = this->model,
                    &task = this->task, &state = this->state,
                    &time = this->time, &mocap = this->mocap, horizon, i]() {
      // scale improvement
      mju_addScl(candidate_policy[i].parameters.data(),
                  candidate_policy[i].parameters.data(),
                  candidate_policy[i].parameter_update.data(),
                  improvement_step[i],
                  model->nu * candidate_policy[i].num_spline_points);

      // policy
      auto feedback_policy = [&candidate_policy = candidate_policy, i](
                                  double* action, const double* state,
                                  double time) {
        candidate_policy[i].Action(action, state, time);
      };

      // policy rollout
      trajectory[i].Rollout(feedback_policy, task, model,
                            data[ThreadPool::WorkerId()].get(), state.data(),
                            time, mocap.data(), horizon);
    });
  }
  pool.WaitCount(count_before + num_trajectory);
  pool.ResetCount();
}

// return trajectory with best total return
const Trajectory* GradientPlanner::BestTrajectory() {
  return &trajectory[winner];
}

// visualize candidate traces in GUI
void GradientPlanner::Traces(mjvScene* scn) {
  // sample color
  float color[4];
  color[0] = 1.0;
  color[1] = 1.0;
  color[2] = 1.0;
  color[3] = 1.0;

  // sample width
  double width = GetNumberOrDefault(0.01, model, "agent_sample_width");

  // scratch
  double zero3[3] = {0};
  double zero9[9] = {0};

  // best
  auto best = this->BestTrajectory();

  for (int k = 0; k < num_trajectory; k++) {
    // plot sample
    for (int i = 0; i < best->horizon - 1; i++) {
      if (scn->ngeom >= scn->maxgeom) continue;
      // initialize geometry
      mjv_initGeom(&scn->geoms[scn->ngeom], mjGEOM_LINE, zero3, zero3, zero9,
                   color);

      // make geometry
      mjv_makeConnector(
          &scn->geoms[scn->ngeom], mjGEOM_LINE, width,
          trajectory[k].trace[3 * i], trajectory[k].trace[3 * i + 1],
          trajectory[k].trace[3 * i + 2], trajectory[k].trace[3 * (i + 1)],
          trajectory[k].trace[3 * (i + 1) + 1],
          trajectory[k].trace[3 * (i + 1) + 2]);

      // increment number of geometries
      scn->ngeom += 1;
    }
  }
}

// planner-specific GUI elements
void GradientPlanner::GUI(mjUI& ui) {
  mjuiDef defGradientPlanner[] = {
      {mjITEM_SLIDERINT, "Rollouts", 2, &num_trajectory, "0 1"},
      // {mjITEM_RADIO, "Action Lmt.", 2, &settings.action_limits, "Off\nOn"},
      // {mjITEM_SLIDERINT, "Iterations", 2, &settings.max_rollout, "1 128"},
      {mjITEM_SELECT, "Spline", 2, &policy.representation,
       "Zero\nLinear\nCubic"},
      {mjITEM_SLIDERINT, "Spline Pts", 2, &policy.num_spline_points, "0 1"},
      // {mjITEM_SLIDERNUM, "Spline Pow. ", 2, &timestep_power, "0 10"},
      {mjITEM_END}};

  // set number of trajectory slider limits
  mju::sprintf_arr(defGradientPlanner[0].other, "%i %i", 1, kMaxTrajectory);

  // set spline point limits
  mju::sprintf_arr(defGradientPlanner[2].other, "%i %i",
                   kMinGradientSplinePoints, kMaxGradientSplinePoints);

  // add gradient descent planner
  mjui_add(&ui, defGradientPlanner);
}

// planner-specific plots
void GradientPlanner::Plots(mjvFigure* fig_planner, mjvFigure* fig_timer,
                            int planning) {
  // bounds
  double planner_bounds[2] = {-6, 6};

  // ----- planner ----- //
  // step size
  mjpc::PlotUpdateData(
      fig_planner, planner_bounds, fig_planner->linedata[0][0] + 1,
      mju_log10(mju_max(step_size, 1.0e-6)), 100, 0, 0, 1, -100);

  // // improvement
  // mjpc::PlotUpdateData(
  //     fig_planner, planner_bounds, fig_planner->linedata[1][0] + 1,
  //     mju_log10(mju_max(improvement, 1.0e-6)), 100, 1, 0, 1, -100);

  // // expected
  // mjpc::PlotUpdateData(
  //     fig_planner, planner_bounds, fig_planner->linedata[2][0] + 1,
  //     mju_log10(mju_max(expected, 1.0e-6)), 100, 2, 0, 1, -100);

  // // surprise
  // mjpc::PlotUpdateData(
  //     fig_planner, planner_bounds, fig_planner->linedata[3][0] + 1,
  //     mju_log10(mju_max(surprise, 1.0e-6)), 100, 3, 0, 1, -100);

  // legend
  mju::strcpy_arr(fig_planner->linename[0], "Step Size");
  // mju::strcpy_arr(fig_planner->linename[1], "Improvement");
  // mju::strcpy_arr(fig_planner->linename[2], "Expected");
  // mju::strcpy_arr(fig_planner->linename[3], "Surprise");

  // ranges
  fig_planner->range[1][0] = planner_bounds[0];
  fig_planner->range[1][1] = planner_bounds[1];

  // ----- timer ----- //
  double timer_bounds[2] = {0.0, 1.0};

  // update plots
  PlotUpdateData(fig_timer, timer_bounds, fig_timer->linedata[9][0] + 1,
                 1.0e-3 * nominal_compute_time * planning, 100, 9, 0, 1, -100);

  PlotUpdateData(fig_timer, timer_bounds, fig_timer->linedata[10][0] + 1,
                 1.0e-3 * model_derivative_compute_time * planning, 100, 10, 0,
                 1, -100);

  PlotUpdateData(fig_timer, timer_bounds, fig_timer->linedata[11][0] + 1,
                 1.0e-3 * cost_derivative_compute_time * planning, 100, 11, 0,
                 1, -100);

  PlotUpdateData(fig_timer, timer_bounds, fig_timer->linedata[12][0] + 1,
                 1.0e-3 * gradient_compute_time * planning, 100, 12, 0, 1,
                 -100);

  PlotUpdateData(fig_timer, timer_bounds, fig_timer->linedata[13][0] + 1,
                 1.0e-3 * rollouts_compute_time * planning, 100, 13, 0, 1,
                 -100);

  PlotUpdateData(fig_timer, timer_bounds, fig_timer->linedata[14][0] + 1,
                 1.0e-3 * policy_update_compute_time * planning, 100, 14, 0, 1,
                 -100);

  // legend
  mju::strcpy_arr(fig_timer->linename[9], "Nominal");
  mju::strcpy_arr(fig_timer->linename[10], "Model Deriv.");
  mju::strcpy_arr(fig_timer->linename[11], "Cost Deriv.");
  mju::strcpy_arr(fig_timer->linename[12], "Gradient");
  mju::strcpy_arr(fig_timer->linename[13], "Rollouts");
  mju::strcpy_arr(fig_timer->linename[14], "Policy Update");
}

}  // namespace mjpc
