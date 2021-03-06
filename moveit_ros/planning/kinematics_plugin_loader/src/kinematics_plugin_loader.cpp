/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Willow Garage, Inc.
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
 *   * Neither the name of Willow Garage nor the names of its
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

/* Author: Ioan Sucan, Dave Coleman */

#include <moveit/kinematics_plugin_loader/kinematics_plugin_loader.h>
#include <moveit/rdf_loader/rdf_loader.h>
#include <pluginlib/class_loader.hpp>
#include <boost/thread/mutex.hpp>
#include <sstream>
#include <vector>
#include <map>
#include <memory>
#include "rclcpp/rclcpp.hpp"
#include <moveit/profiler/profiler.h>

namespace kinematics_plugin_loader
{
rclcpp::Logger LOGGER_KINEMATICS_PLUGIN_LOADER = rclcpp::get_logger("kinematics_plugin_loader");

class KinematicsPluginLoader::KinematicsLoaderImpl
{
public:
  /**
   * \brief Pimpl Implementation of KinematicsLoader
   * \param robot_description
   * \param possible_kinematics_solvers
   * \param search_res
   * \param iksolver_to_tip_links - a map between each ik solver and a vector of custom-specified tip link(s)
   */
  KinematicsLoaderImpl(const std::string& robot_description,
                       const std::map<std::string, std::vector<std::string> >& possible_kinematics_solvers,
                       const std::map<std::string, std::vector<double> >& search_res,
                       const std::map<std::string, std::vector<std::string> >& iksolver_to_tip_links)
    : robot_description_(robot_description)
    , possible_kinematics_solvers_(possible_kinematics_solvers)
    , search_res_(search_res)
    , iksolver_to_tip_links_(iksolver_to_tip_links)
  {
    try
    {
      kinematics_loader_.reset(new pluginlib::ClassLoader<kinematics::KinematicsBase>("moveit_core", "kinematics::"
                                                                                                     "KinematicsBase"));
    }
    catch (pluginlib::PluginlibException& e)
    {
      RCLCPP_ERROR(LOGGER_KINEMATICS_PLUGIN_LOADER, "Unable to construct kinematics loader. Error: %s", e.what());
    }
    if(robot_description_.length()<1){
      robot_description_ = std::string("robot_description");
    }
  }

  /**
   * \brief Helper function to decide which, and how many, tip frames a planning group has
   * \param jmg - joint model group pointer
   * \return tips - list of valid links in a planning group to plan for
   */
  std::vector<std::string> chooseTipFrames(const robot_model::JointModelGroup* jmg)
  {
    std::vector<std::string> tips;
    std::map<std::string, std::vector<std::string> >::const_iterator ik_it =
        iksolver_to_tip_links_.find(jmg->getName());

    // Check if tips were loaded onto the rosparam server previously
    if (ik_it != iksolver_to_tip_links_.end())
    {
      // the tip is being chosen based on a corresponding rosparam ik link
      RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "Choosing tip frame of kinematic solver for group %s"
                                  "based on values in rosparam server.",jmg->getName().c_str());
      tips = ik_it->second;
    }
    else
    {
      // get the last link in the chain
      RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "Choosing tip frame of kinematic solver for group"
                                  "based on last link in chain",jmg->getName().c_str());

      tips.push_back(jmg->getLinkModels().back()->getName());
    }

    // Error check
    if (tips.empty())
    {
      RCLCPP_ERROR(LOGGER_KINEMATICS_PLUGIN_LOADER, "Error choosing kinematic solver tip frame(s).");
    }

    // Debug tip choices
    std::stringstream tip_debug;
    tip_debug << "Planning group '" << jmg->getName() << "' has tip(s): ";
    for (std::size_t i = 0; i < tips.size(); ++i)
      tip_debug << tips[i] << ", ";
    RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, tip_debug.str());

    return tips;
  }

  kinematics::KinematicsBasePtr allocKinematicsSolver(const robot_model::JointModelGroup* jmg)
  {
    kinematics::KinematicsBasePtr result;
    if (!kinematics_loader_)
    {
      RCLCPP_ERROR(LOGGER_KINEMATICS_PLUGIN_LOADER, "Invalid kinematics loader.");
      return result;
    }
    if (!jmg)
    {
      RCLCPP_ERROR(LOGGER_KINEMATICS_PLUGIN_LOADER, "Specified group is NULL. Cannot allocate kinematics solver.");
      return result;
    }
    const std::vector<const robot_model::LinkModel*>& links = jmg->getLinkModels();
    if (links.empty())
    {
      RCLCPP_ERROR(LOGGER_KINEMATICS_PLUGIN_LOADER, "No links specified for group '%s'. Cannot allocate kinematics solver.",
                      jmg->getName().c_str());
      return result;
    }

    RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "Trying to allocate kinematics solver for group '%s'", jmg->getName().c_str());

    std::map<std::string, std::vector<std::string> >::const_iterator it =
        possible_kinematics_solvers_.find(jmg->getName());
    if (it == possible_kinematics_solvers_.end())
    {
      RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "No kinematics solver available for this group");
      return result;
    }

    const std::string& base = links.front()->getParentJointModel()->getParentLinkModel() ?
                                  links.front()->getParentJointModel()->getParentLinkModel()->getName() :
                                  jmg->getParentModel().getModelFrame();

    // just to be sure, do not call the same pluginlib instance allocation function in parallel
    boost::mutex::scoped_lock slock(lock_);
    for (std::size_t i = 0; !result && i < it->second.size(); ++i)
    {
      try
      {
        result = kinematics_loader_->createUniqueInstance(it->second[i]);
        if (result)
        {
          // choose the tip of the IK solver
          const std::vector<std::string> tips = chooseTipFrames(jmg);

          // choose search resolution
          double search_res = search_res_.find(jmg->getName())->second[i];  // we know this exists, by construction

          if (!result->initialize(jmg->getParentModel(), jmg->getName(),
                                  (base.empty() || base[0] != '/') ? base : base.substr(1), tips, search_res) &&
              // on failure: fallback to old method (TODO: remove in future)
              !result->initialize(robot_description_, jmg->getName(),
                                  (base.empty() || base[0] != '/') ? base : base.substr(1), tips, search_res))
          {
            RCLCPP_ERROR(LOGGER_KINEMATICS_PLUGIN_LOADER, "Kinematics solver of type '%s' could not be initialized for group '%s'",
                            it->second[i].c_str(), jmg->getName().c_str());
            result.reset();
            continue;
          }

          result->setDefaultTimeout(jmg->getDefaultIKTimeout());
          RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER,
                          "Successfully allocated and initialized a kinematics solver of type '%s' with search "
                          "resolution %lf for group '%s' at address %p",
                          it->second[i].c_str(), search_res, jmg->getName().c_str(), result.get());
          break;
        }
      }
      catch (pluginlib::PluginlibException& e)
      {
        RCLCPP_ERROR(LOGGER_KINEMATICS_PLUGIN_LOADER, "The kinematics plugin (%s) failed to load. Error: %s", it->first.c_str(), e.what());
      }
    }

    if (!result)
    {
      RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "No usable kinematics solver was found for this group.\n"
                               "Did you load kinematics.yaml into your node's namespace?");
    }
    return result;
  }

  // cache solver between two consecutive calls
  // first call in RobotModelLoader::loadKinematicsSolvers() is just to check suitability for jmg
  // second call in JointModelGroup::setSolverAllocators() is to actually retrieve the instance for use
  kinematics::KinematicsBasePtr allocKinematicsSolverWithCache(const robot_model::JointModelGroup* jmg)
  {
    boost::mutex::scoped_lock slock(cache_lock_);
    kinematics::KinematicsBasePtr& cached = instances_[jmg];
    if (cached.unique())
      return std::move(cached);  // pass on unique instance

    // create a new instance and store in instances_
    cached = allocKinematicsSolver(jmg);
    return cached;
  }

  void status() const
  {
    for (std::map<std::string, std::vector<std::string> >::const_iterator it = possible_kinematics_solvers_.begin();
         it != possible_kinematics_solvers_.end(); ++it){
      for (std::size_t i = 0; i < it->second.size(); ++i){
        RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "Solver for group '%s': '%s' (search resolution = %lf)", it->first.c_str(),
                       it->second[i].c_str(), search_res_.at(it->first)[i]);
       }
     }
  }

private:
  std::string robot_description_;
  std::map<std::string, std::vector<std::string> > possible_kinematics_solvers_;
  std::map<std::string, std::vector<double> > search_res_;
  std::map<std::string, std::vector<std::string> > iksolver_to_tip_links_;  // a map between each ik solver and a vector
                                                                            // of custom-specified tip link(s)
  std::shared_ptr<pluginlib::ClassLoader<kinematics::KinematicsBase> > kinematics_loader_;
  std::map<const robot_model::JointModelGroup*, kinematics::KinematicsBasePtr> instances_;
  boost::mutex lock_;
  boost::mutex cache_lock_;
};

void KinematicsPluginLoader::status() const
{
  if (loader_)
    loader_->status();
  else
    RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "Loader function was never required");
}

robot_model::SolverAllocatorFn KinematicsPluginLoader::getLoaderFunction(std::shared_ptr<rclcpp::Node>& node)
{
  moveit::tools::Profiler::ScopedStart prof_start;
  moveit::tools::Profiler::ScopedBlock prof_block("KinematicsPluginLoader::getLoaderFunction");

  if (loader_)
    return boost::bind(&KinematicsLoaderImpl::allocKinematicsSolverWithCache, loader_.get(), _1);

  rdf_loader::RDFLoader rml(node, robot_description_);
  robot_description_ = rml.getRobotDescription();
  return getLoaderFunction(rml.getSRDF(), node);
}

robot_model::SolverAllocatorFn KinematicsPluginLoader::getLoaderFunction(const srdf::ModelSharedPtr& srdf_model, std::shared_ptr<rclcpp::Node>& node)
{
  moveit::tools::Profiler::ScopedStart prof_start;
  moveit::tools::Profiler::ScopedBlock prof_block("KinematicsPluginLoader::getLoaderFunction(SRDF)");

  if(robot_description_.length()<1){
    robot_description_ = std::string("robot_description");
  }

  if (!loader_)
  {
    RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "Configuring kinematics solvers");
    groups_.clear();

    std::map<std::string, std::vector<std::string> > possible_kinematics_solvers;
    std::map<std::string, std::vector<double> > search_res;
    std::map<std::string, std::vector<std::string> > iksolver_to_tip_links;

    if (srdf_model)
    {
      const std::vector<srdf::Model::Group>& known_groups = srdf_model->getGroups();
      if (default_search_resolution_ <= std::numeric_limits<double>::epsilon())
        default_search_resolution_ = kinematics::KinematicsBase::DEFAULT_SEARCH_DISCRETIZATION;

      if (default_solver_plugin_.empty())
      {
        RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "Loading settings for kinematics solvers from the ROS param server ...");

        // read data using ROS params
        // ros::NodeHandle nh("~");
        //TODO (anasarrak): Add the appropriate node name for ROS2
        auto ksolver_params = std::make_shared<rclcpp::SyncParametersClient>(node, "/dummy_joint_states");
        // read the list of plugin names for possible kinematics solvers
        for (std::size_t i = 0; i < known_groups.size(); ++i)
        {
          std::string base_param_name = known_groups[i].name_;
          RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "Looking for param %s ", (base_param_name + ".kinematics_solver").c_str());
          std::string ksolver_param_name;
          bool found = false;
          // if (ksolver_params->has_parameter(base_param_name + ".kinematics_solver"))
          // {
          //   found = ksolver_params->get_parameter(base_param_name + ".kinematics_solver", ksolver_param_name);
          //   RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "param i s%s ", ksolver_param_name.c_str());
          //
          // }
          if (!found)
          {
            base_param_name = robot_description_ + "_kinematics." + known_groups[i].name_;
            RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "Looking for param %s ", (base_param_name + ".kinematics_solver").c_str());

            auto parameters_and_prefixes = ksolver_params->get_parameters({ base_param_name + ".kinematics_solver" });
            for (auto & value : parameters_and_prefixes) {
                ksolver_param_name = value.value_to_string();
                found = true;
                std::stringstream ss(ksolver_param_name);
                bool first = true;
                while (ss.good() && !ss.eof())
                {
                  if (first)
                  {
                    first = false;
                    groups_.push_back(known_groups[i].name_);
                  }
                  std::string solver;
                  ss >> solver >> std::ws;
                  possible_kinematics_solvers[known_groups[i].name_].push_back(solver);
                  RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "Using kinematics solver '%s' for group '%s'.", solver.c_str(),
                                  known_groups[i].name_.c_str());
                }
            }
            // rclcpp::Parameter param =  ksolver_params->get_parameter(base_param_name + ".kinematics_solver");
            // ksolver_param_name = ksolver_params->get_parameter(base_param_name + ".kinematics_solver").get_value<std::string>();
            // found = ksolver_params->get_parameter(base_param_name + ".kinematics_solver", parameter);
            RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "%d param is %s ", found, ksolver_param_name.c_str());

          }
          // if (found)
          // {
          //   RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "Found param %s", ksolver_param_name.c_str());
          //   std::string ksolver;
          //   if (ksolver_params->has_parameter(ksolver_param_name))
          //   {
          //     ksolver = node->get_parameter(ksolver_param_name).get_value<std::string>();
          //
          //   }
          // }

          std::string ksolver_timeout_param_name;
          if (ksolver_params->has_parameter(base_param_name + "/kinematics_solver_timeout"))
          {
            ksolver_timeout_param_name = node->get_parameter(base_param_name + "/kinematics_solver_timeout").get_value<std::string>();
            double ksolver_timeout;
            if (ksolver_params->has_parameter(ksolver_timeout_param_name)){
              ksolver_timeout = node->get_parameter(ksolver_timeout_param_name).get_value<double>();
              ik_timeout_[known_groups[i].name_] = ksolver_timeout;
            }
            else
            {  // just in case this is an int
              int ksolver_timeout_i;
              if (ksolver_params->has_parameter(ksolver_timeout_param_name))
                ksolver_timeout_i = node->get_parameter(ksolver_timeout_param_name).get_value<int>();
                ik_timeout_[known_groups[i].name_] = ksolver_timeout_i;
            }
          }

          // TODO: Remove in future release (deprecated in PR #1288, Jan-2019, Melodic)
          std::string ksolver_attempts_param_name;
          if (ksolver_params->has_parameter(base_param_name + "/kinematics_solver_attempts"))
          {
            ksolver_attempts_param_name = node->get_parameter(base_param_name + "/kinematics_solver_attempts").get_value<std::string>();
            RCLCPP_WARN(LOGGER_KINEMATICS_PLUGIN_LOADER, "Kinematics solver doesn't support #attempts anymore, but only a timeout.\n"
                                         "Please remove the parameter '%s' from your configuration.",
                                ksolver_attempts_param_name.c_str());
          }

          std::string ksolver_res_param_name;
          if (ksolver_params->has_parameter(base_param_name + "/kinematics_solver_search_resolution"))
          {
            ksolver_res_param_name = node->get_parameter(base_param_name + "/kinematics_solver_search_resolution").get_value<std::string>();
            std::string ksolver_res;
            if (ksolver_params->has_parameter(ksolver_res_param_name))
            {
              ksolver_res = node->get_parameter(ksolver_res_param_name).get_value<std::string>();
              std::stringstream ss(ksolver_res);
              while (ss.good() && !ss.eof())
              {
                double res;
                ss >> res >> std::ws;
                search_res[known_groups[i].name_].push_back(res);
              }
            }
            else
            {  // handle the case this param is just one value and parsed as a double
              if (ksolver_params->has_parameter(ksolver_res_param_name)){
                if(node->get_parameter(ksolver_res_param_name).get_type_name().compare("integer") == 0){
                  int res_i = node->get_parameter(ksolver_res_param_name).get_value<int>();
                  search_res[known_groups[i].name_].push_back(res_i);
                }else{
                  double res = node->get_parameter(ksolver_res_param_name).get_value<double>();
                  search_res[known_groups[i].name_].push_back(res);
                }
              }
            }
          }

          // Allow a kinematic solver's tip link to be specified on the rosparam server
          // Depreciated in favor of array version now
          std::string ksolver_ik_link_param_name;
          if (ksolver_params->has_parameter(base_param_name + "/kinematics_solver_ik_link"))
          {
            ksolver_ik_link_param_name = node->get_parameter(
                        base_param_name + "/kinematics_solver_ik_link").get_value<std::string>();
            std::string ksolver_ik_link;
            if (ksolver_params->has_parameter(ksolver_ik_link_param_name))  // has a custom rosparam-based tip link
            {
              ksolver_ik_link = node->get_parameter(ksolver_ik_link_param_name).get_value<std::string>();
              RCLCPP_WARN(LOGGER_KINEMATICS_PLUGIN_LOADER, "Using kinematics_solver_ik_link rosparam is "
                                             "deprecated in favor of kinematics_solver_ik_links "
                                             "rosparam array.");
              iksolver_to_tip_links[known_groups[i].name_].push_back(ksolver_ik_link);
            }
          }

          // Allow a kinematic solver's tip links to be specified on the rosparam server as an array
          std::string ksolver_ik_links_param_name;
          if (ksolver_params->has_parameter(base_param_name + "/kinematics_solver_ik_links"))
          {
            ksolver_ik_links_param_name = node->get_parameter(
                                base_param_name + "/kinematics_solver_ik_links").get_value<std::string>();
            // XmlRpc::XmlRpcValue ksolver_ik_links;
            std::vector<std::string> ksolver_ik_links;
            if(ksolver_params->has_parameter(ksolver_ik_links_param_name)){
              ksolver_ik_links = node->get_parameter(ksolver_ik_links_param_name).get_value<std::vector<std::string>>();
              if(node->get_parameter(ksolver_ik_links_param_name).get_type_name().find("array")){
                RCLCPP_WARN(LOGGER_KINEMATICS_PLUGIN_LOADER, "the parameter '%s' needs to be of type 'Array'",
                                          ksolver_ik_links_param_name.c_str());
              }else{
                for (int32_t j = 0; j < ksolver_ik_links.size(); ++j)
                {
                  // ROS_ASSERT(ksolver_ik_links[j].getType() == XmlRpc::XmlRpcValue::TypeString);
                  //TODO (anasarrak): does this make sense?
                  assert(typeid(ksolver_ik_links[j]).name() == "string" );
                  RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "found tip %s for group %s", static_cast<std::string>(ksolver_ik_links[j]).c_str(),
                                            known_groups[i].name_.c_str());
                  iksolver_to_tip_links[known_groups[i].name_].push_back(static_cast<std::string>(ksolver_ik_links[j]));
                }
              }
            }
          }

          // make sure there is a default resolution at least specified for every solver (in case it was not specified
          // on the param server)
          while (search_res[known_groups[i].name_].size() < possible_kinematics_solvers[known_groups[i].name_].size())
            search_res[known_groups[i].name_].push_back(default_search_resolution_);
        }
      }
      else
      {
        RCLCPP_INFO(LOGGER_KINEMATICS_PLUGIN_LOADER, "Using specified default settings for kinematics solvers ...");
        for (std::size_t i = 0; i < known_groups.size(); ++i)
        {
          possible_kinematics_solvers[known_groups[i].name_].resize(1, default_solver_plugin_);
          search_res[known_groups[i].name_].resize(1, default_search_resolution_);
          ik_timeout_[known_groups[i].name_] = default_solver_timeout_;
          groups_.push_back(known_groups[i].name_);
        }
      }
    }

    loader_.reset(
        new KinematicsLoaderImpl(robot_description_, possible_kinematics_solvers, search_res, iksolver_to_tip_links));
  }

  return boost::bind(&KinematicsPluginLoader::KinematicsLoaderImpl::allocKinematicsSolverWithCache, loader_.get(), _1);
}
}  // namespace kinematics_plugin_loader
