#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "json.hpp"
#include "spline.h"

//>> pparthas: Some constants used for path planning
#define LNWDTH 		4.0 //given lane width = 4 meters
#define TIMESTEP 	0.02 //time steps are for every 20 ms
#define SAFEGAP 	30 //Safe trailing gap from preceeding car
#define PASSGAP 	20 //Safe gap for passing safely
#define SAFE_ACC_STEP 	0.224 //Safe speed change (acceleration/deceleration) in TIMESTEP to not exceed Jerk Limits (5 meters/sec-squared)
#define SPEEDLMT	49.5 //Speed limit set slightly lower than actual speed limit of 50 mph
#define MPH_2_mps	2.24 //Constant to convert from MPH to meters per second
//<< pparthas

using namespace std;

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.find_first_of("}");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

double distance(double x1, double y1, double x2, double y2)
{
	return sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1));
}
int ClosestWaypoint(double x, double y, const vector<double> &maps_x, const vector<double> &maps_y)
{

	double closestLen = 100000; //large number
	int closestWaypoint = 0;

	for(int i = 0; i < maps_x.size(); i++)
	{
		double map_x = maps_x[i];
		double map_y = maps_y[i];
		double dist = distance(x,y,map_x,map_y);
		if(dist < closestLen)
		{
			closestLen = dist;
			closestWaypoint = i;
		}

	}

	return closestWaypoint;

}

int NextWaypoint(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{

	int closestWaypoint = ClosestWaypoint(x,y,maps_x,maps_y);

	double map_x = maps_x[closestWaypoint];
	double map_y = maps_y[closestWaypoint];

	double heading = atan2((map_y-y),(map_x-x));

	double angle = fabs(theta-heading);
  angle = min(2*pi() - angle, angle);

  if(angle > pi()/4)
  {
    closestWaypoint++;
  if (closestWaypoint == maps_x.size())
  {
    closestWaypoint = 0;
  }
  }

  return closestWaypoint;
}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int next_wp = NextWaypoint(x,y, theta, maps_x,maps_y);

	int prev_wp;
	prev_wp = next_wp-1;
	if(next_wp == 0)
	{
		prev_wp  = maps_x.size()-1;
	}

	double n_x = maps_x[next_wp]-maps_x[prev_wp];
	double n_y = maps_y[next_wp]-maps_y[prev_wp];
	double x_x = x - maps_x[prev_wp];
	double x_y = y - maps_y[prev_wp];

	// find the projection of x onto n
	double proj_norm = (x_x*n_x+x_y*n_y)/(n_x*n_x+n_y*n_y);
	double proj_x = proj_norm*n_x;
	double proj_y = proj_norm*n_y;

	double frenet_d = distance(x_x,x_y,proj_x,proj_y);

	//see if d value is positive or negative by comparing it to a center point

	double center_x = 1000-maps_x[prev_wp];
	double center_y = 2000-maps_y[prev_wp];
	double centerToPos = distance(center_x,center_y,x_x,x_y);
	double centerToRef = distance(center_x,center_y,proj_x,proj_y);

	if(centerToPos <= centerToRef)
	{
		frenet_d *= -1;
	}

	// calculate s value
	double frenet_s = 0;
	for(int i = 0; i < prev_wp; i++)
	{
		frenet_s += distance(maps_x[i],maps_y[i],maps_x[i+1],maps_y[i+1]);
	}

	frenet_s += distance(0,0,proj_x,proj_y);

	return {frenet_s,frenet_d};

}

// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, const vector<double> &maps_s, const vector<double> &maps_x, const vector<double> &maps_y)
{
	int prev_wp = -1;

	while(s > maps_s[prev_wp+1] && (prev_wp < (int)(maps_s.size()-1) ))
	{
		prev_wp++;
	}

	int wp2 = (prev_wp+1)%maps_x.size();

	double heading = atan2((maps_y[wp2]-maps_y[prev_wp]),(maps_x[wp2]-maps_x[prev_wp]));
	// the x,y,s along the segment
	double seg_s = (s-maps_s[prev_wp]);

	double seg_x = maps_x[prev_wp]+seg_s*cos(heading);
	double seg_y = maps_y[prev_wp]+seg_s*sin(heading);

	double perp_heading = heading-pi()/2;

	double x = seg_x + d*cos(perp_heading);
	double y = seg_y + d*sin(perp_heading);

	return {x,y};

}

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  ifstream in_map_(map_file_.c_str(), ifstream::in);

  string line;
  while (getline(in_map_, line)) {
  	istringstream iss(line);
  	double x;
  	double y;
  	float s;
  	float d_x;
  	float d_y;
  	iss >> x;
  	iss >> y;
  	iss >> s;
  	iss >> d_x;
  	iss >> d_y;
  	map_waypoints_x.push_back(x);
  	map_waypoints_y.push_back(y);
  	map_waypoints_s.push_back(s);
  	map_waypoints_dx.push_back(d_x);
  	map_waypoints_dy.push_back(d_y);
  }


//>>pparthas: Initialize lane position and reference velocity 
//start in lane 1
int lane = 1;
//start with reference velocity of 0 mph
double ref_vel = 0.0; 
//<<pparthas          	

h.onMessage([&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,&map_waypoints_dx,&map_waypoints_dy,&ref_vel,&lane](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    //auto sdata = string(data).substr(0, length);
    //cout << sdata << endl;
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);
        
        string event = j[0].get<string>();
        
        if (event == "telemetry") 
	{
          // j[1] is the data JSON object          	       	
        	
        	// Main car's localization Data
          	double car_x = j[1]["x"];
          	double car_y = j[1]["y"];
          	double car_s = j[1]["s"];
          	double car_d = j[1]["d"];
          	double car_yaw = j[1]["yaw"];
          	double car_speed = j[1]["speed"];

          	// Previous path data given to the Planner
          	auto previous_path_x = j[1]["previous_path_x"];
          	auto previous_path_y = j[1]["previous_path_y"];
          	// Previous path's end s and d values 
          	double end_path_s = j[1]["end_path_s"];
          	double end_path_d = j[1]["end_path_d"];

          	// Sensor Fusion Data, a list of all other cars on the same side of the road.
          	auto sensor_fusion = j[1]["sensor_fusion"];

          	//>>pparthas: START of Path Planning
          	// Start with 2 "starting" reference points using previous or current car position
		// Add 3 more widely spaced (x,y) waypoints, evenly spaced at 30m
          	// Interpolate these 5 waypoints with a spline to determine trajectory

		//determine number of points in previous path from simulator
       		int prev_size = previous_path_x.size();
       		if (prev_size > 0)
       		{
       			car_s = end_path_s; //if we have previous data, let's start trajectory from it's last location
       		}

		//control variables based on predictions of speed and position of other cars from sensor fusion
       		bool too_close = false;
       		bool leftlanechange = true;
       		bool rightlanechange = true;

		float ego_lane_center = lane*LNWDTH + LNWDTH/2;
       		//For each car reported by sensor fusion
		//sensor_fusion data has format: [ car_id, x, y, vx, vy, s, d]
       		for (int i = 0; i < sensor_fusion.size(); i++)
       		{
       			float d = sensor_fusion[i][6]; //check i-th car's d value
       			//If i-th car is in same lane as Ego car
       			if ( (d < (ego_lane_center+(LNWDTH/2))) && (d > (ego_lane_center-(LNWDTH/2))) )
       			{
       				double vx = sensor_fusion[i][3]; //i-th car's vx value
       				double vy = sensor_fusion[i][4]; //i-th car's vy value
       				double check_speed = sqrt(vx*vx+vy*vy); //check i-th car's speed
       				double check_car_s = sensor_fusion[i][5]; //check i-th car's d value

       				//if using previous data, project s value out to present position
       				check_car_s += ((double)prev_size*0.02*check_speed);

       				//check if s value greater than mine  (preceeding car) and gap is less than SAFEGAP (30 meters)
				if( (check_car_s > car_s) && ((check_car_s-car_s) < SAFEGAP) )
       				{
       					//We are too close to preceeding car and need to take some action
					cout << "TOO CLOSE: Car ahead @ s = " << check_car_s << ", Ego @ s = " << car_s << endl;

       					too_close = true;     
					//Do lane changes if safe to do so
					//If Ego car is in center lane
       					if (lane == 1) //consider shifting to right or left lanes
       					{                  
       						for (int i = 0; i < sensor_fusion.size(); i++)
       						{
       							float d = sensor_fusion[i][6]; //check ith car's d value
       							double check_car_s = sensor_fusion[i][5];
       							if (d > 0 && d < LNWDTH) //car is in left lane
       							{       								
       								if ((check_car_s > (car_s - PASSGAP)) && (check_car_s < (car_s + PASSGAP)))
       								{	
       									cout << "Left check_car_s = " << check_car_s << endl;
       									cout << " car " << i << " too close to change lane" << endl;       
       									leftlanechange = false;
       								}
       							}
       							else if (d > 2*LNWDTH && d < 3*LNWDTH) //car is in right lane
       							{       							
       								if ((check_car_s > (car_s - 20)) && (check_car_s < (car_s + 20)))
       								{
       									cout << "Right check_car_s = " << check_car_s << endl;
       									cout << " car " << i << " too close to change lane" << endl;
       									rightlanechange = false;
       								}
       							}
       						}
						cout << "leftlanechange = " << leftlanechange << endl;
       						cout << "rightlanechange = " << rightlanechange << endl;
						//Change lane variable used in determining spline trajectory's 3 new 30 meter spaced waypoints
       						if(leftlanechange)
       						{
							lane = 0; // shift to left lane. 
       						}
       						if(rightlanechange)
       						{
							lane = 2; // shift to right lane.
       						}
       					} //END of checking If Ego car is in center lane
       					//If Ego car is in left lane
					if(lane == 0) //consider shifting to center lane
					{
						for (int i = 0; i < sensor_fusion.size(); i++)
						{
							float d = sensor_fusion[i][6]; //check ith car's d value
							double check_car_s = sensor_fusion[i][5]; //check ith car's s value
							if (d > LNWDTH && d < 2*LNWDTH) // car is in Center lane
							{ 
								if ((check_car_s > (car_s - PASSGAP)) && (check_car_s < (car_s + PASSGAP)))
								{ 
									cout << "Center check_car_s = " << check_car_s << endl;
									cout << " car " << i << " too close to change lane" << endl;                        
									rightlanechange = false;
								}
							}
						}
	                  			cout << "rightlanechange = " << rightlanechange << endl;
						if(rightlanechange)
						{
							lane = 1; // shift to center lane.
						}
					} //END of checking If Ego car is in left lane
					//If Ego Car is in right lane
					if(lane == 2) //consider shifting to center lane
					{
						for (int i = 0; i < sensor_fusion.size(); i++)
						{
							float d = sensor_fusion[i][6]; //check ith car's d value
							double check_car_s = sensor_fusion[i][5];
							if (d > LNWDTH && d < 2*LNWDTH) // car is in Center lane
							{ 
								if ((check_car_s > (car_s - 20)) && (check_car_s < (car_s + 20)))
								{ 
									cout << "Center check_car_s = " << check_car_s << endl;
									cout << " car " << i << " too close to change lane" << endl;                        
									leftlanechange = false;
								}
							}
						}
						cout << "leftlanechange = " << leftlanechange << endl;
						if(leftlanechange)
						{
							lane = 1; // shift to center lane.
						}
					} //END of checking If Ego car is in right lane              
       				} //END of checking if s value greater than Ego car's s  (preceeding car) and gap is less than SAFEGAP (30 meters)
       			} //END of checking if car is in same lane as Ego car
       		} //END For loop for each car reported by sensor fusion

       		// Speed control
       		if (too_close) // If too close to preceeding car
       		{
       			ref_vel -= SAFE_ACC_STEP; //Decelerate gradually to not exceed Jerk Limits 5 m/s^2 
       		}
       		else if(ref_vel < SPEEDLMT) // If below speed limit
       		{
       			ref_vel += SAFE_ACC_STEP; //Accelerate gradually to not exceed Jerk Limits 5 m/s^2
       		}

		//Create list of widely spaced (x,y) anchors or way points, evenly spaced at SAFEGAP (30 m)
		//We will interpolate these with a spline to create the desired trajectory
		//and later we will fill it in with more points that control speed
		vector<double> ptsx;
		vector<double> ptsy;

          	//First create 2 starting reference points by using previous points (if there are enough of them)
          	//or use the car's current x,y & yaw
          	double ref_x = car_x;
          	double ref_y = car_y;
          	double ref_yaw = deg2rad(car_yaw);
          	//if the previous size is almost empty, use the car's current x,y & yaw as reference
          	if(prev_size < 2)
		{
          		// Creating another point that is backwards in time compared to where the car is at
          		double prev_car_x = car_x - cos(ref_yaw);
          		double prev_car_y = car_y - sin(ref_yaw);
				
          		ptsx.push_back(prev_car_x);
          		ptsx.push_back(ref_x);
				
          		ptsy.push_back(prev_car_y);
          		ptsy.push_back(ref_y);
          	}
          	//use car's previous end point as reference
          	else
		{
          		// redefine ref as prev path end points
          		ref_x = previous_path_x[prev_size-1];
          		ref_y = previous_path_y[prev_size-1];

          		double prev_ref_x = previous_path_x[prev_size-2];
          		double prev_ref_y = previous_path_y[prev_size-2];
          		ref_yaw = atan2(ref_y - prev_ref_y, ref_x - prev_ref_x);

          		// Use two points that make the path tangent to the prev path's end point
          		ptsx.push_back(prev_ref_x);
          		ptsx.push_back(ref_x);

          		ptsy.push_back(prev_ref_y);
          		ptsy.push_back(ref_y);
          	}

          	//So far we have 2 points based on starting reference
		//In Frenet, add 3 more points spaced evenly 30 m ahead of the starting reference
          	vector<double> next_wp0 = getXY(car_s+30,(2+4*lane),map_waypoints_s, map_waypoints_x, map_waypoints_y);
          	vector<double> next_wp1 = getXY(car_s+60,(2+4*lane),map_waypoints_s, map_waypoints_x, map_waypoints_y);
          	vector<double> next_wp2 = getXY(car_s+90,(2+4*lane),map_waypoints_s, map_waypoints_x, map_waypoints_y);

          	ptsx.push_back(next_wp0[0]);
          	ptsx.push_back(next_wp1[0]);
          	ptsx.push_back(next_wp2[0]);

          	ptsy.push_back(next_wp0[1]);
          	ptsy.push_back(next_wp1[1]);
          	ptsy.push_back(next_wp2[1]); 

		//Transforming from global map coordinates to car's local coordinates
		//With this, the last point would be at x=0,y=0, and at 0 degrees angle
          	for(int i=0; i < ptsx.size(); i++)
		{
          		//shift car ref angle to 0 deg
          		double shift_x = ptsx[i] - ref_x;
          		double shift_y = ptsy[i] - ref_y;

          		ptsx[i] = (shift_x*cos(0-ref_yaw) - shift_y*sin(0-ref_yaw));
          		ptsy[i] = (shift_x*sin(0-ref_yaw) + shift_y*cos(0-ref_yaw));
          	}
			
          	//Create a spline
          	tk::spline s;          	

          	//set (x,y) points to the spline. i.e. add x,y points to the spline
          	s.set_points(ptsx,ptsy);

          	//Define the actual (x,y) points that we will use for the path planner
          	vector<double> next_x_vals;
          	vector<double> next_y_vals;

		//Start with all of the previous path points from last time
          	for(int i = 0; i < previous_path_x.size(); ++i)
          	{
          		next_x_vals.push_back(previous_path_x[i]);
          		next_y_vals.push_back(previous_path_y[i]);
          	}
		//Calculate how to break up spline points such that we travel at the desired reference velocity
		//This is from Aaron's "visual aid" from the project walk through video
		double target_x = 30.0; //Pick a distance (along x-axis or angle 0 in local car coordinates), say 30 m
		double target_y = s(target_x); //Corresponding y is obtained from the spline function
		//Distance along car's path is hypotenuse of triangle with target_x as base, target_y as height
		double target_dist = sqrt((target_x)*(target_x) + (target_y)*(target_y));

          	double x_add_on = 0; //Starting value of x

          	//Fill up rest of our path planner after filling it up with prev path points
          	//here we choose to use 50 points in our path planner controls
          	for (int i = 1; i <= 50-previous_path_x.size() ; i++)
          	{          		
          		double N = (target_dist/(TIMESTEP*ref_vel/MPH_2_mps)); 
          		double x_point = x_add_on + (target_dist/N);
          		double y_point = s(x_point);

          		x_add_on = x_point; //Update starting value of x

          		double x_ref = x_point;
          		double y_ref = y_point;

          		//rotate back to normal after rotating it earlier
          		//transform from local coordinates back to global cordinates
          		x_point = (x_ref*cos(ref_yaw)-y_ref*sin(ref_yaw));
          		y_point = (x_ref*sin(ref_yaw)+y_ref*cos(ref_yaw));

			//add back car's starting position
          		x_point += ref_x;
          		y_point += ref_y;

			//add to path planner control points
          		next_x_vals.push_back(x_point);
          		next_y_vals.push_back(y_point);
          	}
          	//<<pparthas: END of Path planning
          	json msgJson;
          	
          	msgJson["next_x"] = next_x_vals;
          	msgJson["next_y"] = next_y_vals;          	

          	auto msg = "42[\"control\","+ msgJson.dump()+"]";          	

          	//this_thread::sleep_for(chrono::milliseconds(1000));
          	ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);          	
          
        } //END of if (event == "telemetry") 
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}


