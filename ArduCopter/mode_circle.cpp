#include "Copter.h"
#include <AP_Mount/AP_Mount.h>

#if MODE_CIRCLE_ENABLED

/*
 * Init and run calls for circle flight mode
 */

// circle_init - initialise circle controller flight mode
bool ModeCircle::init(bool ignore_checks)
{
    speed_changing = false;

    // set speed and acceleration limits
    pos_control->set_max_speed_accel_NE_cm(wp_nav->get_default_speed_NE_cms(), wp_nav->get_wp_acceleration_cmss());
    pos_control->set_correction_speed_accel_NE_cm(wp_nav->get_default_speed_NE_cms(), wp_nav->get_wp_acceleration_cmss());
    pos_control->set_max_speed_accel_U_cm(-get_pilot_speed_dn(), g.pilot_speed_up, g.pilot_accel_z);
    pos_control->set_correction_speed_accel_U_cmss(-get_pilot_speed_dn(), g.pilot_speed_up, g.pilot_accel_z);

    // initialise circle controller including setting the circle center based on vehicle speed
    copter.circle_nav->init();

#if HAL_MOUNT_ENABLED
    // Check if the CIRCLE_OPTIONS parameter have roi_at_center
    if (copter.circle_nav->roi_at_center()) {
        const Vector3p &pos { copter.circle_nav->get_center_NEU_cm() };
        Location circle_center;
        if (!AP::ahrs().get_location_from_origin_offset_NED(circle_center, pos * 0.01)) {
            return false;
        }
        // point at the ground:
        circle_center.set_alt_cm(0, Location::AltFrame::ABOVE_TERRAIN);
        AP_Mount *s = AP_Mount::get_singleton();
        s->set_roi_target(circle_center);
    }
#endif

    // set auto yaw circle mode
    auto_yaw.set_mode(AutoYaw::Mode::CIRCLE);

    return true;
}

// circle_run - runs the circle flight mode
// should be called at 100hz or more
void ModeCircle::run()
{
    // set speed and acceleration limits
    pos_control->set_max_speed_accel_NE_cm(wp_nav->get_default_speed_NE_cms(), wp_nav->get_wp_acceleration_cmss());
    pos_control->set_max_speed_accel_U_cm(-get_pilot_speed_dn(), g.pilot_speed_up, g.pilot_accel_z);

    // Check for any change in params and update in real time
    copter.circle_nav->check_param_change();

    // pilot changes to circle rate and radius
    // skip if in radio failsafe
    if (rc().has_valid_input() && copter.circle_nav->pilot_control_enabled()) {
        // update the circle controller's radius target based on pilot pitch stick inputs
        const float radius_current = copter.circle_nav->get_radius_cm();           // circle controller's radius target, which begins as the circle_radius parameter
        const float pitch_stick = channel_pitch->norm_input_dz();               // pitch stick normalized -1 to 1
        const float nav_speed = copter.wp_nav->get_default_speed_NE_cms();          // copter WP_NAV parameter speed
        const float radius_pilot_change = (pitch_stick * nav_speed) * G_Dt;     // rate of change (pitch stick up reduces the radius, as in moving forward)
        const float radius_new = MAX(radius_current + radius_pilot_change,0);   // new radius target

        if (!is_equal(radius_current, radius_new)) {
            copter.circle_nav->set_radius_cm(radius_new);
        }

        // update the orbicular rate target based on pilot roll stick inputs
#if AP_RC_TRANSMITTER_TUNING_ENABLED
        // skip if using transmitter based tuning knob for circle rate
        if (!copter.being_tuned(TUNING_CIRCLE_RATE)) {
#else
        {
#endif
            const float roll_stick = channel_roll->norm_input_dz();         // roll stick normalized -1 to 1

            if (is_zero(roll_stick)) {
                // no speed change, so reset speed changing flag
                speed_changing = false;
            } else {
                const float rate = copter.circle_nav->get_rate_degs();           // circle controller's rate target, which begins as the circle_rate parameter
                const float rate_current = copter.circle_nav->get_rate_current(); // current adjusted rate target, which is probably different from _rate_degs
                const float rate_pilot_change = (roll_stick * G_Dt);        // rate of change from 0 to 1 degrees per second
                float rate_new = rate_current;                              // new rate target
                if (is_positive(rate)) {
                    // currently moving clockwise, constrain 0 to 90
                    rate_new = constrain_float(rate_current + rate_pilot_change, 0, 90);

                } else if (is_negative(rate)) {
                    // currently moving counterclockwise, constrain -90 to 0
                    rate_new = constrain_float(rate_current + rate_pilot_change, -90, 0);

                } else if (is_zero(rate) && !speed_changing) {
                    // Stopped, pilot has released the roll stick, and pilot now wants to begin moving with the roll stick
                    rate_new = rate_pilot_change;
                }

                speed_changing = true;
                copter.circle_nav->set_rate_degs(rate_new);
            }
        }
    }

    // get pilot desired climb rate (or zero if in radio failsafe)
    float target_climb_rate_cms = get_pilot_desired_climb_rate();

    // get avoidance adjusted climb rate
    target_climb_rate_cms = get_avoidance_adjusted_climbrate_cms(target_climb_rate_cms);

    // if not armed set throttle to zero and exit immediately
    if (is_disarmed_or_landed()) {
        make_safe_ground_handling();
        return;
    }

    // set motors to full range
    motors->set_desired_spool_state(AP_Motors::DesiredSpoolState::THROTTLE_UNLIMITED);

#if AP_RANGEFINDER_ENABLED
    // update the vertical offset based on the surface measurement
    copter.surface_tracking.update_surface_offset();
#endif

    copter.failsafe_terrain_set_status(copter.circle_nav->update_cms(target_climb_rate_cms));
    pos_control->update_U_controller();

    // call attitude controller with auto yaw
    attitude_control->input_thrust_vector_heading(pos_control->get_thrust_vector(), auto_yaw.get_heading());
}

float ModeCircle::wp_distance_m() const
{
    return copter.circle_nav->get_distance_to_target_cm() * 0.01f;
}

float ModeCircle::wp_bearing_deg() const
{
    return degrees(copter.circle_nav->get_bearing_to_target_rad());
}

#endif
