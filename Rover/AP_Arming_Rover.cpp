#include "AP_Arming_Rover.h"
#include "Rover.h"

// perform pre_arm_rc_checks checks
bool AP_Arming_Rover::rc_calibration_checks(const bool display_failure)
{
    // set rc-checks to success if RC checks are disabled
    if (!check_enabled(Check::RC)) {
        return true;
    }

    const RC_Channel *channels[] = {
        rover.channel_steer,
        rover.channel_throttle
    };
    const char *channel_names[] = {"Steer", "Throttle"};

    for (uint8_t i= 0 ; i < ARRAY_SIZE(channels); i++) {
        const RC_Channel *channel = channels[i];
        const char *channel_name = channel_names[i];
        // check if radio has been calibrated
        if (channel->get_radio_min() > RC_Channel::RC_CALIB_MIN_LIMIT_PWM) {
            check_failed(Check::RC, display_failure, "%s radio min too high", channel_name);
            return false;
        }
        if (channel->get_radio_max() < RC_Channel::RC_CALIB_MAX_LIMIT_PWM) {
            check_failed(Check::RC, display_failure, "%s radio max too low", channel_name);
            return false;
        }
    }
    return AP_Arming::rc_calibration_checks(display_failure);
}

// performs pre_arm gps related checks and returns true if passed
bool AP_Arming_Rover::gps_checks(bool display_failure)
{
    if (!require_location &&
        !rover.control_mode->requires_position() &&
        !rover.control_mode->requires_velocity()) {
        // we don't care!
        return true;
    }

    // call parent gps checks
    if (!AP_Arming::gps_checks(display_failure)) {
        return false;
    }

    const AP_AHRS &ahrs = AP::ahrs();

    // always check if inertial nav has started and is ready
    char failure_msg[50] = {};
    if (!ahrs.pre_arm_check(true, failure_msg, sizeof(failure_msg))) {
        check_failed(display_failure, "AHRS: %s", failure_msg);
        return false;
    }

    // check for ekf failsafe
    if (rover.failsafe.ekf) {
        check_failed(display_failure, "EKF failsafe");
        return false;
    }

    // ensure position estimate is ok
    if (!rover.ekf_position_ok()) {
        // vehicle level position estimate checks
        check_failed(display_failure, "Need Position Estimate");
        return false;
    }

    return true;
}

bool AP_Arming_Rover::pre_arm_checks(bool report)
{
    if (armed) {
        // if we are already armed then skip the checks
        return true;
    }

    if (!hal.scheduler->is_system_initialized()) {
        check_failed(report, "System not initialised");
        return false;
    }

    //are arming checks disabled?
    if (checks_to_perform == 0) {
        return mandatory_checks(report);
    }

    if (rover.g2.sailboat.sail_enabled() && !rover.g2.windvane.enabled()) {
        check_failed(report, "Sailing enabled with no WindVane");
        return false;
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wbitwise-instead-of-logical"
    return (AP_Arming::pre_arm_checks(report)
            & motor_checks(report)
#if AP_OAPATHPLANNER_ENABLED
            & oa_check(report)
#endif
            & parameter_checks(report)
            & mode_checks(report));
#pragma clang diagnostic pop
}

bool AP_Arming_Rover::arm_checks(AP_Arming::Method method)
{
    if (method == AP_Arming::Method::RUDDER) {
        // check if arming/disarming allowed from this mode
        if (!rover.control_mode->allows_arming_from_transmitter()) {
            check_failed(true, "Mode not rudder-armable");
            return false;
        }
    }

    //are arming checks disabled?
    if (checks_to_perform == 0) {
        return true;
    }
    return AP_Arming::arm_checks(method);
}

void AP_Arming_Rover::update_soft_armed()
{
    hal.util->set_soft_armed(is_armed() &&
                             hal.util->safety_switch_state() != AP_HAL::Util::SAFETY_DISARMED);
#if HAL_LOGGING_ENABLED
    AP::logger().set_vehicle_armed(hal.util->get_soft_armed());
#endif
}

/*
  arm motors
 */
bool AP_Arming_Rover::arm(AP_Arming::Method method, const bool do_arming_checks)
{
    if (!AP_Arming::arm(method, do_arming_checks)) {
        AP_Notify::events.arming_failed = true;
        return false;
    }

    // Set the SmartRTL home location. If activated, SmartRTL will ultimately try to land at this point
    rover.g2.smart_rtl.set_home(true);

    // initialize simple mode heading
    rover.mode_simple.init_heading();

    // save home heading for use in sail vehicles
    rover.g2.windvane.record_home_heading();

    update_soft_armed();

    send_arm_disarm_statustext("Throttle armed");

    return true;
}

/*
  disarm motors
 */
bool AP_Arming_Rover::disarm(const AP_Arming::Method method, bool do_disarm_checks)
{
    if (method == AP_Arming::Method::RUDDER) {
        if (rover.g2.motors.active()) {
            // can't emit a message here as full-rudder while driving
            // is not uncommon
            return false;
        }
    }

    if (!AP_Arming::disarm(method, do_disarm_checks)) {
        return false;
    }
    if (rover.control_mode != &rover.mode_auto) {
        // reset the mission on disarm if we are not in auto
        rover.mode_auto.mission.reset();
    }

    update_soft_armed();

    send_arm_disarm_statustext("Throttle disarmed");

    return true;
}

#if AP_OAPATHPLANNER_ENABLED
// check object avoidance has initialised correctly
bool AP_Arming_Rover::oa_check(bool report)
{
    char failure_msg[50] = {};
    if (rover.g2.oa.pre_arm_check(failure_msg, ARRAY_SIZE(failure_msg))) {
        return true;
    }

    check_failed(report, "%s", failure_msg);
    return false;
}
#endif  // AP_OAPATHPLANNER_ENABLED

// perform parameter checks
bool AP_Arming_Rover::parameter_checks(bool report)
{
    // success if parameter checks are disabled
    if (!check_enabled(Check::PARAMETERS)) {
        return true;
    }

    // check waypoint speed is positive
    if (!is_positive(rover.g2.wp_nav.get_default_speed())) {
        check_failed(Check::PARAMETERS, report, "WP_SPEED too low");
        return false;
    }

    return true;
}

// check if arming allowed from this mode
bool AP_Arming_Rover::mode_checks(bool report)
{
    //display failure if arming in this mode is not allowed
    if (!rover.control_mode->allows_arming()) {
        check_failed(report, "Mode not armable");
        return false;
    }
    return true;
}

// check motors are ready
bool AP_Arming_Rover::motor_checks(bool report)
{
    bool ret = rover.g2.motors.pre_arm_check(report);

#if HAL_TORQEEDO_ENABLED
    char failure_msg[50] = {};
    AP_Torqeedo *torqeedo = AP_Torqeedo::get_singleton();
    if (torqeedo != nullptr) {
        if (!torqeedo->pre_arm_checks(failure_msg, ARRAY_SIZE(failure_msg))) {
            check_failed(report, "Torqeedo: %s", failure_msg);
            ret = false;
        }
    }
#endif

    return ret;
}
