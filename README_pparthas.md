# Unit 3, Project 1: Path Planning

This is a write up of the code changes done for the **Path Planning** project by **Prasanna Parthasarathy**

## Overview of Path Planning Algorithm
The algorithm used closely follows the algorithm detailed in the project walk through and adds logic to change lanes safely. The outline of the algorithm is:
* Start with Ego car in center lane (`lane = 1`), with speed at zero mph (`ref_vel = 0`)
* Create a Spline Trajectory using 5 anchor points:
  - First create 2 starting reference points by using previous points (if there are enough of them) or use the car's current x,y & yaw (Creating another point that is backwards in time compared to where the car is at)
  - In Frenet, add 3 more points spaced evenly 30 m ahead of the 2 starting reference points
  - Transform the 5 points from global coordinates to local or ego car coordinates
  - Create a spline trajectory using these 5 points
* Define 50 actual control (x,y) points that we will use for the path planner
  - First fill it up with points from previous path (if any)
  - For the rest, calculate how to break up spline points such that we travel at the desired reference velocity. This follows the logic from the walk through video, where a target distance is achieved in N time steps
  - Transform these points back to global coordinates before sending to simulator for path planning control points

## Overview of Handling Traffic and Driving as Fast as Possible
* Start with Ego car in center lane (`lane = 1`), with speed at zero mph (`ref_vel = 0`) and never exceeds speed limit of 50 MPH
* From sensor fusion data determine if car ahead of ego car (in the same lane) is too close (set `too_close = true`). This means we need to take some action like change lanes and slow down
* At each time step:
  - If `too_close == true`, decelerate gradually by subtracting `SAFE_ACC_STEP` from `ref_vel` (calculated as not to exceed Jerk Limits 5 m/s^2), and take **Steps for changing lanes** outlined below OR  
  - If `too_close != true`: accelerate gradually till you come close to the speed limit. So if `ref_vel` is below desired speed defined as `SPEEDLMT` and set to 49.5 MPH, by adding `SAFE_ACC_STEP` to `ref_vel` (calculated as not to exceed Jerk Limits 5 m/s^2)

###### Steps for changing lanes:
* If Ego car is in center lane (`lane == 1`):
  - consider shifting to either left or right lanes
  - if any car is too close to ego car in the left lane, don't change to left lane (set `leftlanechange = false`)
  - else if any car is too close to ego car in right lane, don't change to right lane (set `rightlanechange = false`)
* If ego car is in left lane (`lane == 0`):
  - consider shifting to center lane
  - if any car is too close to ego car in center lane, don't change to center lane (set `rightlanechange = false`)
* If Ego Car is in right lane
  - consider shifting to center lane
  - if any car is too close to ego car in center lane, don't change to center lane (set `leftlanechange = false`)  
