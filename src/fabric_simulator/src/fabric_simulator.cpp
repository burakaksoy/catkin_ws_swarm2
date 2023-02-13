/*
 * Author: Burak Aksoy
 */

#include "fabric_simulator/fabric_simulator.h"

// using namespace std;
using namespace fabric_simulator;

FabricSimulator::FabricSimulator(ros::NodeHandle &nh, ros::NodeHandle &nh_local, boost::recursive_mutex &mtx): 
    nh_(nh), 
    nh_local_(nh_local),
    mtx_(mtx)
{
    p_active_ = false;

    time_frames_ = 0;
    time_sum_ = 0.0;

    is_auto_sim_rate_set_ = false; 

    is_rob_01_attached_ = false;
    is_rob_02_attached_ = false;
    is_rob_03_attached_ = false;
    is_rob_04_attached_ = false;

    rob_01_attached_id_ = -1; //note: ids start from 0. -1 would be a null id.
    rob_02_attached_id_ = -1;
    rob_03_attached_id_ = -1;
    rob_04_attached_id_ = -1;

    // Initialize Timers with deafault period (note: last 2 false mean: oneshot=false, autostart=false)
    timer_render_ = nh_.createTimer(ros::Duration(1.0), &FabricSimulator::render, this,false, false); 
    timer_simulate_ = nh_.createTimer(ros::Duration(1.0), &FabricSimulator::simulate, this,false, false); 

    // Initilize parameters
    params_srv_ = nh_local_.advertiseService("params", &FabricSimulator::updateParams, this);
    initialize();
}

FabricSimulator::~FabricSimulator() {
    nh_local_.deleteParam("gravity_x");
    nh_local_.deleteParam("gravity_y");
    nh_local_.deleteParam("gravity_z");

    nh_local_.deleteParam("dt");
    nh_local_.deleteParam("num_substeps");
    nh_local_.deleteParam("num_steps");

    nh_local_.deleteParam("fabric_x");
    nh_local_.deleteParam("fabric_y");
    nh_local_.deleteParam("fabric_density");
    nh_local_.deleteParam("fabric_resolution");
    nh_local_.deleteParam("fabric_bending_compliance");
    nh_local_.deleteParam("initial_height");

    nh_local_.deleteParam("simulation_rate");
    nh_local_.deleteParam("rendering_rate");

    nh_local_.deleteParam("fabric_points_topic_name");
    nh_local_.deleteParam("fabric_points_frame_id");
    
    nh_local_.deleteParam("odom_01_topic_name");
    nh_local_.deleteParam("odom_02_topic_name");
    nh_local_.deleteParam("odom_03_topic_name");
    nh_local_.deleteParam("odom_04_topic_name");
    
    nh_local_.deleteParam("fabric_rob_z_offset");
}

bool FabricSimulator::updateParams(std_srvs::Empty::Request& req, std_srvs::Empty::Response& res)
{
    bool prev_active = p_active_;

    // Get parameters from the parameter server
    nh_local_.param<bool>("active", p_active_, true);
    nh_local_.param<bool>("reset", p_reset_, false);

    nh_local_.param<double>("gravity_x", gravity_x_, 0.0);
    nh_local_.param<double>("gravity_y", gravity_y_, 0.0);
    nh_local_.param<double>("gravity_z", gravity_z_, -9.81);
    
    nh_local_.param<double>("dt", dt_, 1.0 / 125.0); //90
    nh_local_.param<bool>("set_sim_rate_auto", set_sim_rate_auto_, true); // to set the simulation rate and dt automatically

    nh_local_.param<int>("num_substeps", num_substeps_, 3); //3
    nh_local_.param<int>("num_steps", num_steps_, 1);
    
    nh_local_.param<double>("fabric_x", fabric_x_, 2.); //2
    nh_local_.param<double>("fabric_y", fabric_y_, 2.); //2
    nh_local_.param<double>("fabric_density", fabric_density_, 5);
    nh_local_.param<double>("fabric_resolution", fabric_resolution_, 10); //10
    nh_local_.param<double>("fabric_bending_compliance", fabric_bending_compliance_, 1.0);
    nh_local_.param<double>("initial_height", initial_height_, 1.0);
    
    nh_local_.param<double>("simulation_rate", simulation_rate_, 90.0); //90
    nh_local_.param<double>("rendering_rate", rendering_rate_, 30.0); //30

    nh_local_.param<std::string>("fabric_points_topic_name", fabric_points_topic_name_, std::string("cloth_points"));
    nh_local_.param<std::string>("fabric_points_frame_id", fabric_points_frame_id_, std::string("map"));

    nh_local_.param<std::string>("odom_01_topic_name", odom_01_topic_name_, std::string("d1/ground_truth/odom"));
    nh_local_.param<std::string>("odom_02_topic_name", odom_02_topic_name_, std::string("d2/ground_truth/odom"));
    nh_local_.param<std::string>("odom_03_topic_name", odom_03_topic_name_, std::string("d3/ground_truth/odom"));
    nh_local_.param<std::string>("odom_04_topic_name", odom_04_topic_name_, std::string("d4/ground_truth/odom"));

    nh_local_.param<double>("fabric_rob_z_offset_", fabric_rob_z_offset_, 1.0);

    // Set timer periods based on the parameters
    timer_render_.setPeriod(ros::Duration(1.0/rendering_rate_));
    timer_simulate_.setPeriod(ros::Duration(1.0/simulation_rate_));

    // Initilize gravity vector
    gravity_ << gravity_x_, gravity_y_, gravity_z_;    

    //Create mesh
    std::string fabric_name = "cloth";
    pbd_object::Mesh fabric_mesh = FabricSimulator::createMeshRectangular(fabric_name ,fabric_x_, fabric_y_, initial_height_, fabric_resolution_);

    // std::cout << "fabric_mesh.name: " << fabric_mesh.name << std::endl;
    // std::cout << "fabric_mesh.vertices:\n" << fabric_mesh.vertices << std::endl;
    // std::cout << "fabric_mesh.face_tri_ids:\n" << fabric_mesh.face_tri_ids << std::endl;

    // Create cloth
    fabric_ = pbd_object::Cloth(fabric_mesh, fabric_bending_compliance_, fabric_density_);

    if (p_active_ != prev_active) {
        if (p_active_) {
            // Create subscribers
            sub_odom_01_ = nh_.subscribe(odom_01_topic_name_, 1, &FabricSimulator::odometryCb_01, this);
            sub_odom_02_ = nh_.subscribe(odom_02_topic_name_, 1, &FabricSimulator::odometryCb_02, this);
            sub_odom_03_ = nh_.subscribe(odom_03_topic_name_, 1, &FabricSimulator::odometryCb_03, this);
            sub_odom_04_ = nh_.subscribe(odom_04_topic_name_, 1, &FabricSimulator::odometryCb_04, this);

            // Create publishers
            pub_fabric_points_ = nh_.advertise<visualization_msgs::Marker>(fabric_points_topic_name_, 1);

            // Start timers
            timer_simulate_.start();
            timer_render_.start();
        }
        else {
            // Send empty message?

            // Stop subscribers
            sub_odom_01_.shutdown();
            sub_odom_02_.shutdown();
            sub_odom_03_.shutdown();
            sub_odom_04_.shutdown();

            // Stop publishers
            pub_fabric_points_.shutdown();

            // Stop timers
            timer_render_.stop();
            timer_simulate_.stop();
        }
    }

    if (p_reset_)
        reset();

    return true;
}

void FabricSimulator::reset(){
    time_frames_ = 0;
    time_sum_ = 0.0;

    is_auto_sim_rate_set_ = false; 

    is_rob_01_attached_ = false;
    is_rob_02_attached_ = false;
    is_rob_03_attached_ = false;
    is_rob_04_attached_ = false;

    rob_01_attached_id_ = -1; //note: ids start from 0. -1 would be a null id.
    rob_02_attached_id_ = -1;
    rob_03_attached_id_ = -1;
    rob_04_attached_id_ = -1;

    p_reset_ = false;
    nh_local_.setParam("reset",false);
}

pbd_object::Mesh FabricSimulator::createMeshRectangular(const std::string &name, const double &fabric_x, const double &fabric_y, const double &fabric_z, const double &fabric_res){
    // Create vector of 3D Eigen vectors to hold the "list of vertices" and "list of face triangle ids"
    std::vector<Eigen::RowVector3d> vertices;
    std::vector<Eigen::RowVector3i> face_tri_ids;

    int num_particle_x = fabric_x * fabric_res;
    int num_particle_y = fabric_y * fabric_res;
    // Assuming a fabric centered at the origin, create a linear spaced coordinate vectors for the coordinates
    
    // Generate the x_coords
    Eigen::RowVectorXd x_coords(num_particle_x + 1);
    double start = fabric_x/2.0;
    double end = -fabric_x/2.0;
    double step = (end - start) / num_particle_x;

    for (int i = 0; i <= num_particle_x; i++) {
        x_coords(i) = start + i * step;
    }
    
    // Generate the y_coords
    Eigen::RowVectorXd y_coords(num_particle_y + 1);
    start = fabric_y/2.0;
    end = -fabric_y/2.0;
    step = (end - start) / num_particle_y;

    for (int i = 0; i <= num_particle_y; i++) {
        y_coords(i) = start + i * step;
    }

    // Create vertices with x,y,z coordinates
    for (int i = 0; i < x_coords.size(); i++) {
        for (int j = 0; j < y_coords.size(); j++) {
            Eigen::RowVector3d v(x_coords(i), y_coords(j), fabric_z);
            vertices.push_back(v);
        }
    }

    // Eigen::Map<Eigen::MatrixXd> vertices_mat((double *)vertices.data(), vertices.size(), 3);
    // (double *)vertices.data() returns a pointer to the first element of the vertices vector, 
    // which can be cast to a double pointer. The Eigen::Map object takes the pointer, the 
    // number of rows vertices.size(), and the number of columns 3, as arguments, and maps this 
    // memory block to an Eigen::MatrixXd object.
    Eigen::MatrixXd vertices_mat(vertices.size(), 3);
    for (int i = 0; i < vertices.size(); i++) {
        vertices_mat.row(i) = vertices[i];
    }



    // Create face triangle ids
    int id = 0;
    for (int i = 0; i < x_coords.size() - 1; i++) {
        for (int j = 0; j < y_coords.size() - 1; j++) {
            Eigen::RowVector3i ids(id, id + 1, id + y_coords.size());
            face_tri_ids.push_back(ids);

            if (j > 0) {
                Eigen::RowVector3i ids(id, id + y_coords.size(), id + y_coords.size() - 1);
                face_tri_ids.push_back(ids);
            }

            if (j + 1 == y_coords.size() - 1) {
                Eigen::RowVector3i ids(id + 1, id + 1 + y_coords.size(), id + 1 + y_coords.size() - 1);
                face_tri_ids.push_back(ids);
                id++;
            }
            id++;
        }
    }

    // Eigen::Map<Eigen::MatrixXi> face_tri_ids_mat((int *)face_tri_ids.data(), face_tri_ids.size(), 3);
    Eigen::MatrixXi face_tri_ids_mat(face_tri_ids.size(), 3);
    for (int i = 0; i < face_tri_ids.size(); i++) {
        face_tri_ids_mat.row(i) = face_tri_ids[i];
    }


    pbd_object::Mesh mesh;
    mesh.name = name;
    mesh.vertices = vertices_mat;
    mesh.face_tri_ids = face_tri_ids_mat;

    return mesh;
}

void FabricSimulator::simulate(const ros::TimerEvent& e){
    // With some kind of self lock to prevent collision with rendering
    boost::recursive_mutex::scoped_lock lock(mtx_);

    double sdt = dt_ / num_substeps_;

    // std::cout << "sdt: " << sdt << std::endl; 

    // std::chrono::high_resolution_clock::time_point start_time = high_resolution_clock::now();
    ros::Time start_time = ros::Time::now();

    // Small steps implementation
    // -------------------------------
    for (int i = 0; i< num_steps_; i++){
        for (int j = 0; j < num_substeps_; j++){
            
            fabric_.preSolve(sdt,gravity_);
            fabric_.solve(sdt);
            fabric_.postSolve(sdt);
        }
    }
    // -------------------------------

    // std::chrono::high_resolution_clock::time_point finish_time = high_resolution_clock::now();
    ros::Time finish_time = ros::Time::now();
    
    // double elapsed_time = duration_cast<microseconds>(finish_time - start_time).count() * 0.000001;
    ros::Duration elapsed_time = finish_time - start_time;

    time_sum_ += elapsed_time.toSec();
    time_frames_ += 1;

    if (time_frames_ > 10) {
        time_sum_ /= time_frames_;
        
        // std::cout << std::fixed << std::setprecision(3) << time_sum_ << " s per frame" << std::endl;
        // time_sum_str = to_string(time_sum_);
        // ROS_INFO("[Fabric Simulator]: %s s per frame",time_sum_str.c_srt());
        ROS_INFO("[Fabric Simulator]: %lf secs per frame", time_sum_);

        // Smart dt and simulation rate selection
        if (!is_auto_sim_rate_set_ && set_sim_rate_auto_) {
            dt_ = time_sum_;
            timer_simulate_.setPeriod(ros::Duration(time_sum_));
            is_auto_sim_rate_set_ = true; 
        }

        time_frames_ = 0;
        time_sum_ = 0;
    }

    // Eigen::MatrixX3d *pos_ptr = fabric_.getPosPtr();
    // Eigen::MatrixX2i *stretching_ids_ptr = fabric_.getStretchingIdsPtr();
    // drawRviz(pos_ptr);
    // drawRvizWireframe(pos_ptr,stretching_ids_ptr);
}

void FabricSimulator::render(const ros::TimerEvent& e){
    // With some kind of self lock to prevent collision with simulation
    boost::recursive_mutex::scoped_lock lock(mtx_);
    
    // Publish RVIZ visualization markers here
    // Eigen::MatrixX3d pos = fabric_.getPos();
    // Eigen::MatrixX3d stretching_ids = fabric_.getStretchingIds();
    // drawRviz(pos);
    // drawRvizWireframe(pos,stretching_ids);

    Eigen::MatrixX3d *pos_ptr = fabric_.getPosPtr();
    Eigen::MatrixX2i *stretching_ids_ptr = fabric_.getStretchingIdsPtr();
    drawRviz(pos_ptr);
    drawRvizWireframe(pos_ptr,stretching_ids_ptr);
}

void FabricSimulator::drawRviz(const Eigen::MatrixX3d *poses){
    std::vector<geometry_msgs::Point> clothRVIZPoints;

    for (int i = 0; i < poses->rows(); i++) {
        geometry_msgs::Point p;
        p.x = (*poses)(i, 0);
        p.y = (*poses)(i, 1);
        p.z = (*poses)(i, 2);

        clothRVIZPoints.push_back(p);
    }

    publishRvizPoints(clothRVIZPoints);
}

void FabricSimulator::drawRvizWireframe(const Eigen::MatrixX3d *poses, const Eigen::MatrixX2i *ids){
    // objects: *poses, *ids
    std::vector<geometry_msgs::Point> clothRVIZEdges;

    for (int i = 0; i < ids->rows(); i++) {
        int id0 = (*ids)(i, 0);
        int id1 = (*ids)(i, 1);

        geometry_msgs::Point p1;
        p1.x = (*poses)(id0, 0);
        p1.y = (*poses)(id0, 1);
        p1.z = (*poses)(id0, 2);
        clothRVIZEdges.push_back(p1);

        geometry_msgs::Point p2;
        p2.x = (*poses)(id1, 0);
        p2.y = (*poses)(id1, 1);
        p2.z = (*poses)(id1, 2);
        clothRVIZEdges.push_back(p2);
    }

    publishRvizLines(clothRVIZEdges);
}

void FabricSimulator::publishRvizPoints(const std::vector<geometry_msgs::Point> &points){
    visualization_msgs::Marker m;

    m.header.frame_id = fabric_points_frame_id_;
    m.header.stamp = ros::Time::now();

    m.type = visualization_msgs::Marker::POINTS;
    m.id = 0;
    m.action = visualization_msgs::Marker::ADD;

    m.pose.orientation.w = 1.0;

    m.points = points;

    m.scale.x = 0.01;
    m.scale.y = 0.01;
    m.scale.z = 0.01;

    m.color.a = 1.;
    m.color.r = 1.;
    m.color.g = 0.5;
    m.color.b = 0.;

    pub_fabric_points_.publish(m);
}

void FabricSimulator::publishRvizLines(const std::vector<geometry_msgs::Point> &points){
    visualization_msgs::Marker m;

    m.header.frame_id = fabric_points_frame_id_;
    m.header.stamp = ros::Time::now();

    m.type = visualization_msgs::Marker::LINE_LIST;
    m.id = 1;
    m.action = visualization_msgs::Marker::ADD;

    m.pose.orientation.w = 1.0;

    m.points = points;

    // LINE_STRIP/LINE_LIST markers use only the x component of scale, for the line width
    m.scale.x = 0.005;

    m.color.a = 1.;
    m.color.r = 0.;
    m.color.g = 1.0;
    m.color.b = 0.;

    pub_fabric_points_.publish(m);
}

void FabricSimulator::odometryCb_01(const nav_msgs::Odometry::ConstPtr odom_msg){
    double x = odom_msg->pose.pose.position.x;
    double y = odom_msg->pose.pose.position.y;
    double z = odom_msg->pose.pose.position.z + fabric_rob_z_offset_;
    
    Eigen::RowVector3d pos(x, y, z);

    if (!is_rob_01_attached_)
    {
        // tell sim objects (fabric) to attach robot to the nearest particles
        rob_01_attached_id_ = fabric_.attachNearest(pos);
        // std::cout << "self.rob_01_attached_id, " << rob_01_attached_id_ << std::endl;

        if (rob_01_attached_id_ != -1)
        {
            is_rob_01_attached_ = true;
        }
    }
    else
    {
        // tell sim object to update its position
            fabric_.updateAttachedPose(rob_01_attached_id_, pos);
    }
}

void FabricSimulator::odometryCb_02(const nav_msgs::Odometry::ConstPtr odom_msg){
    double x = odom_msg->pose.pose.position.x;
    double y = odom_msg->pose.pose.position.y;
    double z = odom_msg->pose.pose.position.z + fabric_rob_z_offset_;
    
    Eigen::RowVector3d pos(x, y, z);

    if (!is_rob_02_attached_)
    {
        // tell sim objects (fabric) to attach robot to the nearest particles
        rob_02_attached_id_ = fabric_.attachNearest(pos);
        // std::cout << "self.rob_02_attached_id, " << rob_02_attached_id_ << std::endl;

        if (rob_02_attached_id_ != -1)
        {
            is_rob_02_attached_ = true;
        }
    }
    else
    {
        // tell sim object to update its position
            fabric_.updateAttachedPose(rob_02_attached_id_, pos);
    }
}

void FabricSimulator::odometryCb_03(const nav_msgs::Odometry::ConstPtr odom_msg){
    double x = odom_msg->pose.pose.position.x;
    double y = odom_msg->pose.pose.position.y;
    double z = odom_msg->pose.pose.position.z + fabric_rob_z_offset_;
    
    Eigen::RowVector3d pos(x, y, z);

    if (!is_rob_03_attached_)
    {
        // tell sim objects (fabric) to attach robot to the nearest particles
        rob_03_attached_id_ = fabric_.attachNearest(pos);
        // std::cout << "self.rob_03_attached_id, " << rob_03_attached_id_ << std::endl;

        if (rob_03_attached_id_ != -1)
        {
            is_rob_03_attached_ = true;
        }
    }
    else
    {
        // tell sim object to update its position
            fabric_.updateAttachedPose(rob_03_attached_id_, pos);
    }
}

void FabricSimulator::odometryCb_04(const nav_msgs::Odometry::ConstPtr odom_msg){
    double x = odom_msg->pose.pose.position.x;
    double y = odom_msg->pose.pose.position.y;
    double z = odom_msg->pose.pose.position.z + fabric_rob_z_offset_;
    
    Eigen::RowVector3d pos(x, y, z);

    if (!is_rob_04_attached_)
    {
        // tell sim objects (fabric) to attach robot to the nearest particles
        rob_04_attached_id_ = fabric_.attachNearest(pos);
        // std::cout << "self.rob_04_attached_id, " << rob_04_attached_id_ << std::endl;

        if (rob_04_attached_id_ != -1)
        {
            is_rob_04_attached_ = true;
        }
    }
    else
    {
        // tell sim object to update its position
            fabric_.updateAttachedPose(rob_04_attached_id_, pos);
    }
}