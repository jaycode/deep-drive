#include <iostream>
#include "vehicle.h"
#include "json.hpp"
#include "helpers.h"
#include "spline.h"
#include <float.h>
#include <math.h>
#include <tuple>
#include "cost.h"

using json = nlohmann::json;

using namespace ego;
using namespace ego_help;

/**
 * Initialize Vehicle
 */
EgoCar::EgoCar(World world, Position position, EgoConfig config) {
  this->state = STATE_KL;
  this->position = position;
  this->config = config;
  this->world = world;
}

OtherCar::OtherCar(World world, Position position, OtherConfig config) {
  this->world = world;
  this->position = position;
  this->config = config;
}

EgoCar::~EgoCar() {}
OtherCar::~OtherCar() {}

Snapshot EgoCar::getSnapshot() {
  Snapshot snap;
  snap.state = this->state;
  snap.world = this->world;
  snap.position = this->position;
  snap.config = this->config;
  return snap;
}

void EgoCar::InitFromSnapshot(const Snapshot &snap) {
  this->state = snap.state;
  this->world = snap.world;
  this->position = snap.position;
  this->config = snap.config;
}

Trajectory EgoCar::PlanTrajectory(const vector<OtherCar> &other_cars,
                                  const ego::CostWeights &weights) {
  Trajectory t;
  State best_state = this->ChooseBestState(other_cars, weights, &t);
  this->state = best_state;
  return t;
}

int debug_iter = 0;
State EgoCar::ChooseBestState(const vector<OtherCar> &other_cars,
                              const ego::CostWeights &weights,
                              Trajectory *best_t) {
  double best_cost = DBL_MAX;
  State best_state;

  // Go through the states one by one and calculate the cost for walking down
  // each state.
  Snapshot initial_snap = this->getSnapshot();
  Snapshot best_snap;

  Trajectory current_best_t;

  this->config.unsure_frenet = false;
  for ( int state = 0; state != ENUM_END; state++ ) {
    // cout << "Now calculating cost of state " << State2Str((State)state) << endl;
    Snapshot cur_snap = this->getSnapshot();
    // For each state, the ego vehicle "imagines" following a trajectory
    Trajectory trajectory = this->CreateTrajectory((State)state, other_cars, &cur_snap);

    // cout << "trajectory last s,d: " << trajectory.s[trajectory.s.size()-1] <<
    //   ", " << trajectory.d[trajectory.d.size()-1] << endl;

    // TODO: Looks like reinforcement learning can be implemented here
    //       by treating (state + other_cars) as a state
    tuple<State, Snapshot, vector<OtherCar>> cf_state = 
      make_tuple(State(state), cur_snap, other_cars);

    double cost = ego_cost::CalculateCost(cf_state, trajectory, weights);
    if (cost < best_cost) {
      best_state = (State)state;
      current_best_t = trajectory;
      best_cost = cost;
      best_snap = cur_snap;
    }
    // cout << "Cost of state " << State2Str((State)state) << ": " << cost << endl << endl;
    this->InitFromSnapshot(initial_snap);
    // if ((State)state == STATE_LCR) {
    //   cout << "Final trajectory point's lane: " << d2lane(trajectory.d[trajectory.d.size()-1]) << endl;
    // }
  }

  cout << endl << "Chosen State: " << State2Str((State)best_state) << endl << "-----" << endl << endl;
  (*best_t) = current_best_t;
  this->InitFromSnapshot(best_snap);
  // ++debug_iter;
  // if (debug_iter == 1) {
  //   exit(0);
  // }
  // cout << "chosen target speed: " << this->config.target_speed << endl;
  // cout << "Target lane: " << this->config.target_lane << " or in d: " << lane2d(this->config.target_lane) << endl;

  // if ((State)best_state != STATE_KL) {
  //   exit(0);
  // }
  return (State)best_state;
}

Trajectory EgoCar::CreateTrajectory(State state,
                                    const vector<OtherCar> &other_cars,
                                    Snapshot *snap) {
  Trajectory t;
  vector<double> x = {};
  vector<double> y = {};
  vector<double> s = {};
  vector<double> d = {};
  t.x = x;
  t.y = y;
  t.s = s;
  t.d = d;
  t.distance = 0.0;

  vector<double> &map_waypoints_s = *this->world.map_waypoints_s;
  vector<double> &map_waypoints_x = *this->world.map_waypoints_x;
  vector<double> &map_waypoints_y = *this->world.map_waypoints_y;

  json &previous_path_x = *(*snap).config.previous_path_x;
  json &previous_path_y = *(*snap).config.previous_path_y;

  int prev_size = previous_path_x.size();
  // if (prev_size > (*snap).config.num_last_path) {
  //   prev_size = (*snap).config.num_last_path;
  // }
  Position ref = (*snap).position;

  double &car_length = (*snap).config.car_length;

  // Local-coordinates of waypoints (i.e. car position is [0,0])
  vector<double> localwp_x;
  vector<double> localwp_y;

  // cout << "Checking conversion accuracy" << endl;
  // cout << "Car s, d converted into x, y: " << endl;
  // vector<double> cur_xy = getXY(car_s, car_d,
  //                                map_waypoints_s, map_waypoints_x,
  //                                map_waypoints_y);
  // cout << cur_xy[0] << ", " << cur_xy[1] << endl;

  if (prev_size < 2) {
    // Create the initial two waypoints.
    // This is important otherwise the car would jump to nowhere.

    // Calculate previous position i.e. the position of
    // rear wheels.
    double prev_car_x = ref.x - car_length * cos(ref.yaw);
    double prev_car_y = ref.y - car_length * sin(ref.yaw);

    localwp_x.push_back(prev_car_x);
    localwp_x.push_back(ref.x);
    localwp_y.push_back(prev_car_y);
    localwp_y.push_back(ref.y);
  }
  else {
    // For subsequent steps, continue from previous path.
    ref.x = previous_path_x[prev_size-1];
    ref.y = previous_path_y[prev_size-1];

    double prev_ref_x = previous_path_x[prev_size-2];
    double prev_ref_y = previous_path_y[prev_size-2];
    ref.yaw = atan2(ref.y - prev_ref_y, ref.x - prev_ref_x);

    if (ref.x > prev_ref_x) {
      localwp_x.push_back(prev_ref_x);
      localwp_x.push_back(ref.x);
      localwp_y.push_back(prev_ref_y);
      localwp_y.push_back(ref.y);
    }
  }
  vector<double> ref_sd = getFrenet(ref.x, ref.y, ref.yaw,
                                    map_waypoints_x, map_waypoints_y);
  ref.s = ref_sd[0];
  ref.d = ref_sd[1];

  // From this point on, use reference positions to decide on things.
  // In other words, make decisions based on the last point in waypoint.
  // Switch state step
  switch(state) {
    case STATE_KL: {
      this->RealizeKeepLane(other_cars, ref, snap);
      break;
    }
    case STATE_LCL: {
      if (d2lane(ref.d) > 0) {
        this->RealizeLaneChange(other_cars, -1, ref, snap);
      }
      break;
    }
    case STATE_FC: {
      this->RealizeFollowCar(other_cars, ref, snap);
      break;
    }
    case STATE_LCR: {
      if (d2lane(ref.d) < 2) {
        this->RealizeLaneChange(other_cars, 1, ref, snap);
      }
      break;
    }
    default: {

    }
  }

  ref.v = (*snap).config.target_speed;

  // ===PARAMETERS===
  // All these parameters will have been changed by the switch state step above.
  // They are used in changing the trajectory.
 
  // Current velocity in meter/second.
  double &cur_v = (*snap).position.v;

  // double ref_yaw = 0.0;

  // Current lane: 0 - left, 1 - center, 2 - right
  int &lane = (*snap).config.target_lane;

  // Number of waypoints.
  int &num_wp = (*snap).config.num_wp;

  // Spline anchors
  int &spline_anchors = (*snap).config.spline_anchors;
  double anchor_distance = ((*snap).config.horizon / spline_anchors);

  double &dt = (*snap).config.dt;

  double max_a = (*snap).config.max_acceleration;

  // ===END===
  
  // Place anchor points. They are located ahead of the car.
  // cout << "Before pushing anchors, num of x: " << localwp_x.size() << endl;
  // cout << "x values:" << endl;
  // for (int i=0; i<localwp_x.size(); ++i) {
  //   cout << localwp_x[i] << endl;
  // }

  double conservative_distance = 3.0;
  double conservative_v = 5.0;
  // double conservative_a = 0.5;
  // cout << "Set lane to " << lane << " (" << lane2d(lane) << ")" << endl;
  for (int i = 1; i <= spline_anchors; ++i) {
    vector<double> next_xy = getXY(ref.s+i*anchor_distance, lane2d(lane),
                                   map_waypoints_s, map_waypoints_x,
                                   map_waypoints_y);

    vector<double> next_sd = getFrenet(next_xy[0], next_xy[1],
                                   ref.yaw, map_waypoints_x,
                                   map_waypoints_y);

    // cout << "Next_sd's lane from d: " << d2lane(next_sd[1]) << 
    //   " ("<< next_sd[1] << ")" << endl;
    // Compare sd and xy distances:
    double x2 = next_xy[0];
    double y2 = next_xy[1];
    if (localwp_x.size() > 0) {
      // cout << "sd distance: " << anchor_distance << endl;
      double x1 = localwp_x[localwp_x.size()-1];
      double y1 = localwp_y[localwp_y.size()-1];    
      double xydist = distance(x1,y1,x2,y2);
      // cout << "xy distance: " << xydist << endl;

      double ddist = (xydist - anchor_distance);
      if ((*snap).config.anchor_ddist_threshold < ddist) {
        // cout << "I am unsure if Frenet conversion was correct (distance > "
        //  << (*snap).config.anchor_ddist_threshold << ")" << endl;
        this->config.unsure_frenet = true;
        // x2 = x1 + (i * conservative_distance * cos(ref.yaw));
        // y2 = y1 + (i * conservative_distance * sin(ref.yaw));
        // cout << "Original [x,y] (" << x1 << "," << y1 << ") " <<
        //   "was added by " << (i*conservative_distance) << " with car heading " <<
        //   ref.yaw << " resulting in new [x,y] = (" << x2 << "," << y2 << ")" <<
        //   endl;
        // ref.v = conservative_v;
        // (*snap).config.target_x = 2.0;
      }
    }

    localwp_x.push_back(x2);
    localwp_y.push_back(y2);
    // cout << "place y2 at " << y2 << " (lane " << lane << ")" << endl;
  }

  // Shift and rotate reference to 0 degree and origin coordinate.
  for (int i = 0; i < localwp_x.size(); ++i) {
    double shift_x = localwp_x[i] - ref.x;
    double shift_y = localwp_y[i] - ref.y;
    localwp_x[i] = (shift_x * cos(0 - ref.yaw) - shift_y * sin(0 - ref.yaw));
    localwp_y[i] = (shift_x * sin(0 - ref.yaw) + shift_y * cos(0 - ref.yaw));
  }

  // Speed trajectory. We assume that the speed
  // follows a linear trajectory. From the calculation below
  // we get the total required acceleration to reach target speed.
  double total_time = dt * num_wp;
  double dv = ref.v - cur_v;
  double req_accel = dv / total_time;
  // cout << "n leftover waypoints: " << prev_size << endl;
  // cout << "car position [x,y]: [" << ref.x << ", " << ref.y <<
  //         "]"<< endl;
  // cout << "current v: " << cur_v << " target v: " << ref.v << endl;
  // cout << "total time (sec): " << total_time << endl;
  // cout << "required acceleration: " << req_accel << endl;
  // Since the car needs to adhere to a maximum acceleration,
  // we substract max acceleration from required acceleration
  // in each second.

  // Spline for position
  tk::spline spline_pos;

  PruneWaypoint(&localwp_x, &localwp_y);

  // There are some problems with the trajectory e.g.
  // x points are not sorted or there are duplicates.
  // This could be caused by the car running too
  // slow.
  // cout << "Car speed at error: " << cur_v << endl;

  // cout << "num of x: " << localwp_x.size() << endl;
  // cout << "x values:" << endl;
  // for (int i=0; i<localwp_x.size(); ++i) {
  //   cout << localwp_x[i] << endl;
  // }
  // cout << "stdev of local x and y: " << stdev(localwp_x) <<
  //   " | " << stdev(localwp_y) << endl;

  spline_pos.set_points(localwp_x, localwp_y);

  // Re-include previous waypoints if any.
  for (int i = 0; i < prev_size; ++i) {
    t.x.push_back(previous_path_x[i]);
    t.y.push_back(previous_path_y[i]);
    vector<double> point_sd = getFrenet(previous_path_x[i],
                                        previous_path_y[i], ref.yaw,
                                        map_waypoints_x, map_waypoints_y);
    t.s.push_back(point_sd[0]);
    t.d.push_back(point_sd[1]);
    if (i > 0) {
      t.distance += distance(t.x[i-1], t.y[i-1], t.x[i], t.y[i]);
    }
  }

  // cout << "prev_size: " << prev_size << endl;
  // cout << "size of prev path: " << previous_path_x.size() << endl;
  // // cout << "previous path s distance: " << (t.s[prev_size-1] - t.s[0]) << endl;

  // Create target position in front of the car
  double target_x = (*snap).config.target_x;
  double target_y = spline_pos(target_x);
  double target_dist = sqrt((target_x) * (target_x) + (target_y) * (target_y));

  /**
   * The path between the car and the target contains several points.
   * In the code below we place these points onto this path.
   * One thing to note here is that the car has a maximum acceleration
   * it can use, so there is no guarantee that the car
   * will reach the target distance specified above. 
   */ 
  // x distance traveled so far in the loop.
  double x_so_far = 0;
  for (int i = 0; i < num_wp - prev_size; ++i) {
    double v;
    if (dv < 0) {
      // cout << "decelerate by " << (dt * max_a) << endl;
      v = cur_v - (dt * i * max_a);
    }
    else {
      // cout << "accelerate by " << (dt * max_a) << endl;
      v = min(cur_v + (dt * i * max_a), ref.v);
    }
    double point_dist = (dt * v);
    double point_x = min((x_so_far + point_dist), target_x);
    // cout << "point_x: " << point_x << endl;

    // We do not want to predict any points beyond the target.
    // This is useful for later cost calculation, to decide
    // which path travels the farthest.
    // cout << "x so far: " << x_so_far << endl;
    if (x_so_far <= target_x) {
      double point_y = spline_pos(point_x);

      x_so_far = point_x;

      // Rotate back to world coordinates.
      double temp_x = point_x;
      double temp_y = point_y;
      point_x = (temp_x * cos(ref.yaw) - temp_y * sin(ref.yaw));
      point_y = (temp_x * sin(ref.yaw) + temp_y * cos(ref.yaw));

      point_x += ref.x;
      point_y += ref.y;

      // cout << endl << "point_x is " << point_x << endl; 
      vector<double> point_sd = getFrenet(point_x, point_y, ref.yaw,
                                          map_waypoints_x, map_waypoints_y);
      t.x.push_back(point_x);
      t.y.push_back(point_y);
      t.s.push_back(point_sd[0]);
      t.d.push_back(point_sd[1]);
      t.distance += point_dist;
    }
    else {
      cout << i << endl;
      break;
    }
  }

  // if (state == STATE_LCR) {
  //   cout << "stored tj lane: " << d2lane(t.d[t.d.size()-1]) << endl;
  //   debug_iter++;
  //   if (debug_iter==50) {
  //     exit(0);
  //   }
  // }

  // cout << "Current car's lane: " << d2lane(this->position.d) << endl;
  // cout << "target lane: " << (*snap).config.target_lane << endl;
  // cout << "target_x: " << target_x << endl;
  // cout << "target dist: " << target_dist << endl;
  // cout << "max a: " << max_a << " target_v: " << ref.v << endl;
  cout << "ref.x: " << ref.x << " ref.y: " << ref.y << " ref.yaw " << ref.yaw << endl;
  cout << "ref.s: " << ref.s << " ref.d: " << ref.d << endl;
  cout << "car.x: " << this->position.x << " car.y: " << this->position.y << endl;
  cout << "car.s: " << this->position.s << " car.d: " << this->position.d << endl;

  // The code below shows the difference between trailing distance and calculated
  // from s. The difference was almost 10 meters!
  // double s1 = t.s[t.s.size()-1];
  // double s2 = t.s[0];
  // double dist = (s1 - s2);
  // cout << "After creation, trajectory dist (from s|trailing): " <<
  //   dist << "|" << t.distance << endl;
  // Output for 1st run:
  // After creation, trajectory dist (from s|trailing): 35.1848|16.929

  // cout << "Number of trajectory points: " << t.s.size() << endl;

  // cout << endl << "trajectory [x, y]: " << endl;
  // for (int i; i < t.x.size(); ++i) {
  //   cout << t.x[i] << ", " << t.y[i] << endl;
  // }
  // cout << endl;

  return t;
}

void EgoCar::RealizeKeepLane(const vector<OtherCar> &other_cars,
                             const Position &ref, Snapshot *snap) {
  (*snap).config.max_acceleration = (*snap).config.default_max_acceleration;
  (*snap).config.target_speed = (*snap).config.default_target_speed;
  (*snap).config.target_lane = d2lane(ref.d);
}

void EgoCar::RealizeFollowCar(const vector<OtherCar> &other_cars,
                              const Position &ref, Snapshot *snap) {
  // Similar to RealizeKeepLane but find a car to follow.
  double minimum_distance = (*snap).config.follow_distance;
  vector<int> lanes = {d2lane(ref.d)};
  (*snap).config.max_acceleration = config.default_max_acceleration;
  // Set target speed to the car in front of ego car.
  (*snap).config.target_lane = d2lane(ref.d);
  vector<OtherCar> closest_car_v = this->FindClosestCar(
    other_cars, ref, snap, lanes, minimum_distance);
  if (closest_car_v.size() > 0) {
    OtherCar closest_car = closest_car_v[0];
    // cout << "Should set target speed to " << closest_car.position.v << endl;
    (*snap).config.target_x = abs(closest_car.position.s - (*snap).position.s);
    (*snap).config.horizon = abs(closest_car.position.s - (*snap).position.s);
    (*snap).config.target_speed = closest_car.position.v;
    cout << "Set target speed to " << (*snap).config.target_speed << endl;
  }
}

bool EgoCar::is_behind(const OtherCar &car) {
  // TODO: Does higher s always mean in front of the car?
  return (this->position.s < car.position.s);
}

std::vector<OtherCar> EgoCar::FindClosestCar(
  const vector<OtherCar> &other_cars,
  const Position &ref, Snapshot *snap,
  const vector<int> &lanes,
  double minimum_distance = 5.0) {

  // Find the closest car in the same lane and ahead of the ego car.
  bool found_car = false;
  double distance = DBL_MAX;

  OtherCar closest_car;
  for (OtherCar const& car : other_cars) {
    bool in_lanes = false;
    if ( std::find(
      lanes.begin(), lanes.end(), d2lane(car.position.d)) != lanes.end() ) {
      in_lanes = true;
    }
    if (in_lanes && this->is_behind(car)) {
      // Initial setting, register the first car found.
      if (found_car == false) {
        closest_car = car;
      }
      double new_distance = abs(closest_car.position.s - ref.s);

      if (new_distance < distance && new_distance < minimum_distance) {
        closest_car = car;
        found_car = true;
        // cout << "closest car id: " << closest_car.config.id <<
        //         " distance (prev|new): " << distance << "|" <<
        //         new_distance << " car speed: " <<
        //         closest_car.position.v << endl;
        // cout << "s(this|closest car): " << ref.s << "|" <<
        //         closest_car.position.s << endl;
        distance = new_distance;
      }
    }
  }
  vector<OtherCar> v;
  if (found_car == true) {
     v.push_back(closest_car);
  }
  return v;
  
}

void EgoCar::RealizeLaneChange(const vector<OtherCar> &other_cars,
                               int num_lanes, const Position &ref, Snapshot *snap) {
  // If the next line is empty, then it is time to consider line change.

  (*snap).config.target_x = 40.0;
  (*snap).config.horizon = 70.0;
  (*snap).config.target_lane = d2lane((*snap).position.d) + num_lanes;

  // Turning would naturally result in smaller acceleration.
  // TODO: Find this decrease value with the right physics.
  (*snap).config.max_acceleration = 0.60 * (*snap).config.default_max_acceleration;
  (*snap).config.target_speed = 0.80 * (*snap).config.default_target_speed;
}
