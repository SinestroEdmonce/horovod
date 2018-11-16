// Copyright 2018 Martin Krasser. All Rights Reserved.
// Modifications copyright (C) 2018 Uber Technologies, Inc.
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
// =============================================================================

#include "bayesian_optimization.h"

#include <cmath>
#include <iostream>

#include <Eigen/LU>

#include "LBFGS.h"

using Eigen::VectorXd;
using Eigen::MatrixXd;

namespace horovod {
namespace common {

const double NORM_PDF_C = std::sqrt(2 * M_PI);

std::vector<std::uniform_real_distribution<>> GetDistributions(std::vector<std::pair<double, double>> bounds) {
  std::vector<std::uniform_real_distribution<>> dists;
  for (const std::pair<double, double>& bound : bounds) {
    dists.push_back(std::uniform_real_distribution<>(bound.first, bound.second));
  }
  return dists;
}


BayesianOptimization::BayesianOptimization(std::vector<std::pair<double, double>> bounds, double alpha, double xi)
    : d_(bounds.size()),
      bounds_(bounds),
      xi_(xi),
      dists_(GetDistributions(bounds)),
      gpr_(GaussianProcessRegressor(alpha)) {}

void BayesianOptimization::AddSample(const Eigen::VectorXd& x, double y) {
  VectorXd y_v(1);
  y_v << y;
  AddSample(x, y_v);
}

void BayesianOptimization::AddSample(const Eigen::VectorXd& x, const Eigen::VectorXd& y) {
  x_samples_.push_back(x);
  y_samples_.push_back(y);
}

VectorXd BayesianOptimization::NextSample() {
  MatrixXd x_sample(x_samples_.size(), d_);
  for (int i = 0; i < x_samples_.size(); i++) {
    x_sample.row(i) = x_samples_[i];
  }

  MatrixXd y_sample(y_samples_.size(), 1);
  for (int i = 0; i < y_samples_.size(); i++) {
    y_sample.row(i) = y_samples_[i];
  }

  gpr_.Fit(&x_sample, &y_sample);

  return ProposeLocation(x_sample, y_sample);
}

void BayesianOptimization::Clear() {
  x_samples_.clear();
  y_samples_.clear();
}

VectorXd BayesianOptimization::ProposeLocation(const MatrixXd& x_sample, const MatrixXd& y_sample, int n_restarts) {
  auto f = [&](const VectorXd& x) {
    // Minimization objective is the negative acquisition function
    return -ExpectedImprovement(x.transpose(), x_sample)[0];
  };

  auto min_obj = [&](const VectorXd& x, VectorXd& grad) {
    double f0 = CheckBounds(x) ? f(x) : std::numeric_limits<double>::max();
    GaussianProcessRegressor::ApproxFPrime(x, f, f0, grad);
    return f0;
  };

  LBFGSpp::LBFGSParam<double> param;
  param.epsilon = 1e-5;
  param.max_iterations = 100;

  LBFGSpp::LBFGSSolver<double> solver(param);

  VectorXd x_next = VectorXd::Zero(d_);
  double fx_min = std::numeric_limits<double>::max();
  for (int i = 0; i < n_restarts; i++) {
    VectorXd x = VectorXd::Zero(d_);
    for (int j = 0; j < d_; j++) {
      x[j] = dists_[j](gen_);
    }

    VectorXd x0 = x;

    double fx;
    solver.minimize(min_obj, x, fx);

    if (fx < fx_min) {
      fx_min = fx;
      x_next = x;
    }
  }

  return x_next;
}

VectorXd BayesianOptimization::ExpectedImprovement(const MatrixXd& x, const MatrixXd& x_sample) {
  Eigen::VectorXd mu;
  Eigen::VectorXd sigma;
  gpr_.Predict(x, mu, &sigma);

  Eigen::VectorXd mu_sample;
  gpr_.Predict(x_sample, mu_sample);

  // Needed for noise-based model, otherwise use y_sample.maxCoeff.
  // See also section 2.4 in https://arxiv.org/pdf/1012.2599.pdf:
  // Eric Brochu, Vlad M. Cora, Nando de Freitas,
  // A Tutorial on Bayesian Optimization of Expensive Cost Functions
  double mu_sample_opt = mu_sample.maxCoeff();

  auto pdf = [](double x) {
    return std::exp(-(x * x) / 2.0) / NORM_PDF_C;
  };

  auto cdf = [](double x) {
    return 0.5 * std::erfc(-x * M_SQRT1_2);
  };

  Eigen::VectorXd imp = mu.array() - mu_sample_opt - xi_;

  VectorXd z = imp.array() / sigma.array();

  VectorXd ei = imp.cwiseProduct(z.unaryExpr(cdf)) + sigma.cwiseProduct(z.unaryExpr(pdf));
  ei = (sigma.array() != 0).select(ei, 0.0);
  return ei;
}

bool BayesianOptimization::CheckBounds(const Eigen::VectorXd& x) {
  for (int i = 0; i < x.size(); i++) {
    if (x[i] < bounds_[i].first || x[i] > bounds_[i].second) {
      return false;
    }
  }
  return true;
}

} // namespace common
} // namespace horovod
