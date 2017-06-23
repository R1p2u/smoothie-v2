/*
      This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl) with additions from Sungeun K. Jeon (https://github.com/chamnit/grbl)
      Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
      Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
      You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Robot.h"
//#include "libs/Kernel.h"

#include "Planner.h"
#include "Conveyor.h"
#include "Pin.h"
#include "StepperMotor.h"
#include "GCode.h"
#include "StepTicker.h"
#include "BaseSolution.h"
// #include "CartesianSolution.h"
// #include "RotatableCartesianSolution.h"
// #include "LinearDeltaSolution.h"
// #include "RotaryDeltaSolution.h"
// #include "HBotSolution.h"
// #include "CoreXZSolution.h"
// #include "MorganSCARASolution.h"
#include "OutputStream.h"
//#include "StreamOutputPool.h"
//#include "ExtruderPublicAccess.h"
//#include "GcodeDispatch.h"
#include "ActuatorCoordinates.h"

#include <math.h>
#include <string>
#include <algorithm>

#define hypotf(a, b) (sqrtf(powf(a, 2) + powf(b, 2)))

#define  default_seek_rate_key          "default_seek_rate"
#define  default_feed_rate_key          "default_feed_rate"
#define  mm_per_line_segment_key        "mm_per_line_segment"
#define  delta_segments_per_second_key  "delta_segments_per_second"
#define  mm_per_arc_segment_key         "mm_per_arc_segment"
#define  mm_max_arc_error_key           "mm_max_arc_error"
#define  arc_correction_key             "arc_correction"
#define  x_axis_max_speed_key           "x_axis_max_speed"
#define  y_axis_max_speed_key           "y_axis_max_speed"
#define  z_axis_max_speed_key           "z_axis_max_speed"
#define  segment_z_moves_key            "segment_z_moves"
#define  save_g92_key                   "save_g92"
#define  set_g92_key                    "set_g92"

// arm solutions
#define  arm_solution_key               "arm_solution"
#define  cartesian_key                  "cartesian"
#define  rotatable_cartesian_key        "rotatable_cartesian"
#define  rostock_key                    "rostock"
#define  linear_delta_key               "linear_delta"
#define  rotary_delta_key               "rotary_delta"
#define  delta_key                      "delta"
#define  hbot_key                       "hbot"
#define  corexy_key                     "corexy"
#define  corexz_key                     "corexz"
#define  kossel_key                     "kossel"
#define  morgan_key                     "morgan"

// new-style actuator stuff
#define  actuator_key                   "actuator"

#define  step_pin_key                   "step_pin"
#define  dir_pin_key                    "dir_pin"
#define  en_pin_key                     "en_pin"

#define  steps_per_mm_key               "steps_per_mm"
#define  max_rate_key                   "max_rate"
#define  acceleration_key               "acceleration"
#define  z_acceleration_key             "z_acceleration"

#define  alpha_key                      "alpha"
#define  beta_key                       "beta"
#define  gamma_key                      "gamma"

#define ARC_ANGULAR_TRAVEL_EPSILON 5E-7F // Float (radians)
#define PI 3.14159265358979323846F // force to be float, do not use M_PI

// TODO move to utils
static std::string wcs2gcode(int wcs) {
    std::string str= "G5";
    str.append(1, std::min(wcs, 5) + '4');
    if(wcs >= 6) {
        str.append(".").append(1, '1' + (wcs - 6));
    }
    return str;
}

// The Robot converts GCodes into actual movements, and then adds them to the Planner, which passes them to the Conveyor so they can be added to the queue
// It takes care of cutting arcs into segments, same thing for line that are too long

Robot::Robot() : Module("robot")
{
    instance= this;

    this->inch_mode = false;
    this->absolute_mode = true;
    this->e_absolute_mode = true;
    this->select_plane(X_AXIS, Y_AXIS, Z_AXIS);
    memset(this->machine_position, 0, sizeof machine_position);
    memset(this->compensated_machine_position, 0, sizeof compensated_machine_position);
    this->arm_solution = NULL;
    seconds_per_minute = 60.0F;
    this->clearToolOffset();
    this->compensationTransform = nullptr;
    this->get_e_scale_fnc = nullptr;
    this->wcs_offsets.fill(wcs_t(0.0F, 0.0F, 0.0F));
    this->g92_offset = wcs_t(0.0F, 0.0F, 0.0F);
    this->next_command_is_MCS = false;
    this->disable_segmentation = false;
    this->disable_arm_solution = false;
    this->n_motors = 0;
}

#define ACTUATOR_KEYS(X) {     \
    X "_step_pin",        \
    X "_dir_pin",         \
    X "_en_pin",          \
    X "_steps_per_mm",    \
    X "_max_rate",        \
    X "_acceleration"     \
}

bool Robot::configure(ConfigReader& cr)
{
#if 0
    // Arm solutions are used to convert positions in millimeters into position in steps for each stepper motor.
    // While for a cartesian arm solution, this is a simple multiplication, in other, less simple cases, there is some serious math to be done.
    // To make adding those solution easier, they have their own, separate object.
    // Here we read the config to find out which arm solution to use
    if (this->arm_solution) delete this->arm_solution;
    int solution_key = get_key(THEKERNEL->config->value(arm_solution_key)->by_default("cartesian")->as_string());

    if(solution_key == hbot_key || solution_key == corexy_key) {
        this->arm_solution = new HBotSolution(THEKERNEL->config);

    } else if(solution_key == corexz_key) {
        this->arm_solution = new CoreXZSolution(THEKERNEL->config);

    } else if(solution_key == rostock_key || solution_key == kossel_key || solution_key == delta_key || solution_key ==  linear_delta_key) {
        this->arm_solution = new LinearDeltaSolution(THEKERNEL->config);

    } else if(solution_key == rotatable_cartesian_key) {
        this->arm_solution = new RotatableCartesianSolution(THEKERNEL->config);

    } else if(solution_key == rotary_delta_key) {
        this->arm_solution = new RotaryDeltaSolution(THEKERNEL->config);

    } else if(solution_key == morgan_key) {
        this->arm_solution = new MorganSCARASolution(THEKERNEL->config);

    } else if(solution_key == cartesian_key) {
        this->arm_solution = new CartesianSolution(THEKERNEL->config);

    } else {
        this->arm_solution = new CartesianSolution(THEKERNEL->config);
    }

    this->feed_rate           = THEKERNEL->config->value(default_feed_rate_key   )->by_default(  100.0F)->as_number();
    this->seek_rate           = THEKERNEL->config->value(default_seek_rate_key   )->by_default(  100.0F)->as_number();
    this->mm_per_line_segment = THEKERNEL->config->value(mm_per_line_segment_key )->by_default(    0.0F)->as_number();
    this->delta_segments_per_second = THEKERNEL->config->value(delta_segments_per_second_key )->by_default(0.0f   )->as_number();
    this->mm_per_arc_segment  = THEKERNEL->config->value(mm_per_arc_segment_key  )->by_default(    0.0f)->as_number();
    this->mm_max_arc_error    = THEKERNEL->config->value(mm_max_arc_error_key    )->by_default(   0.01f)->as_number();
    this->arc_correction      = THEKERNEL->config->value(arc_correction_key      )->by_default(    5   )->as_number();

    // in mm/sec but specified in config as mm/min
    this->max_speeds[X_AXIS]  = THEKERNEL->config->value(x_axis_max_speed_key    )->by_default(60000.0F)->as_number() / 60.0F;
    this->max_speeds[Y_AXIS]  = THEKERNEL->config->value(y_axis_max_speed_key    )->by_default(60000.0F)->as_number() / 60.0F;
    this->max_speeds[Z_AXIS]  = THEKERNEL->config->value(z_axis_max_speed_key    )->by_default(  300.0F)->as_number() / 60.0F;

    this->segment_z_moves     = THEKERNEL->config->value(segment_z_moves_key     )->by_default(true)->as_bool();
    this->save_g92            = THEKERNEL->config->value(save_g92_key            )->by_default(false)->as_bool();
    string g92                = THEKERNEL->config->value(set_g92_key             )->by_default("")->as_string();
    if(!g92.empty()) {
        // optional setting for a fixed G92 offset
        std::vector<float> t = parse_number_list(g92.c_str());
        if(t.size() == 3) {
            g92_offset = wcs_t(t[0], t[1], t[2]);
        }
    }

    // default s value for laser
    //this->s_value             = THEKERNEL->config->value(laser_module_default_power_key)->by_default(0.8F)->as_number();

    // Make our Primary XYZ StepperMotors, and potentially A B C
    const char* const keys[][6] = {
        ACTUATOR_KEYS("alpha"), // X
        ACTUATOR_KEYS("beta"),  // Y
        ACTUATOR_KEYS("gamma"), // Z
#if MAX_ROBOT_ACTUATORS > 3
        ACTUATOR_KEYS("delta"),   // A
#if MAX_ROBOT_ACTUATORS > 4
        ACTUATOR_KEYS("epsilon"), // B
#if MAX_ROBOT_ACTUATORS > 5
        ACTUATOR_KEYS("zeta")     // C
#endif
#endif
#endif
    };

    // default acceleration setting, can be overriden with newer per axis settings
    this->default_acceleration = THEKERNEL->config->value(acceleration_key)->by_default(100.0F )->as_number(); // Acceleration is in mm/s^2

    // make each motor
    for (size_t a = 0; a < MAX_ROBOT_ACTUATORS; a++) {
        Pin pins[3]; //step, dir, enable
        for (size_t i = 0; i < 3; i++) {
            pins[i].from_string(THEKERNEL->config->value(keys[a][i])->by_default("nc")->as_string())->as_output();
        }

        if(!pins[0].connected() || !pins[1].connected()) { // step and dir must be defined, but enable is optional
            if(a <= Z_AXIS) {
                printf("FATAL: motor %c is not defined in config\n", 'X' + a);
                n_motors = a; // we only have this number of motors
                return;
            }
            break; // if any pin is not defined then the axis is not defined (and axis need to be defined in contiguous order)
        }

        StepperMotor *sm = new StepperMotor(pins[0], pins[1], pins[2]);
        // register this motor (NB This must be 0,1,2) of the actuators array
        uint8_t n = register_motor(sm);
        if(n != a) {
            // this is a fatal error
            printf("FATAL: motor %d does not match index %d\n", n, a);
            return;
        }

        actuators[a]->change_steps_per_mm(THEKERNEL->config->value(keys[a][3])->by_default(a == 2 ? 2560.0F : 80.0F)->as_number());
        actuators[a]->set_max_rate(THEKERNEL->config->value(keys[a][4])->by_default(30000.0F)->as_number() / 60.0F); // it is in mm/min and converted to mm/sec
        actuators[a]->set_acceleration(THEKERNEL->config->value(keys[a][5])->by_default(NAN)->as_number()); // mm/secs²
    }

    check_max_actuator_speeds(); // check the configs are sane

    // if we have not specified a z acceleration see if the legacy config was set
    if(isnan(actuators[Z_AXIS]->get_acceleration())) {
        float acc = THEKERNEL->config->value(z_acceleration_key)->by_default(NAN)->as_number(); // disabled by default
        if(!isnan(acc)) {
            actuators[Z_AXIS]->set_acceleration(acc);
        }
    }

    // initialise actuator positions to current cartesian position (X0 Y0 Z0)
    // so the first move can be correct if homing is not performed
    ActuatorCoordinates actuator_pos;
    arm_solution->cartesian_to_actuator(machine_position, actuator_pos);
    for (size_t i = 0; i < n_motors; i++)
        actuators[i]->change_last_milestone(actuator_pos[i]);

    //this->clearToolOffset();
#endif

    return true;
}

void Robot::on_halt(bool flg)
{
    is_halted= flg;
}

uint8_t Robot::register_motor(StepperMotor *motor)
{
    // register this motor with the step ticker
    StepTicker::getInstance()->register_motor(motor);
    if(n_motors >= k_max_actuators) {
        // this is a fatal error
        printf("FATAL: too many motors, increase k_max_actuators\n");
        return 255;
    }
    actuators.push_back(motor);
    motor->set_motor_id(n_motors);
    return n_motors++;
}

void  Robot::push_state()
{
    bool am = this->absolute_mode;
    bool em = this->e_absolute_mode;
    bool im = this->inch_mode;
    saved_state_t s(this->feed_rate, this->seek_rate, am, em, im, current_wcs);
    state_stack.push(s);
}

void Robot::pop_state()
{
    if(!state_stack.empty()) {
        auto s = state_stack.top();
        state_stack.pop();
        this->feed_rate = std::get<0>(s);
        this->seek_rate = std::get<1>(s);
        this->absolute_mode = std::get<2>(s);
        this->e_absolute_mode = std::get<3>(s);
        this->inch_mode = std::get<4>(s);
        this->current_wcs = std::get<5>(s);
    }
}

std::vector<Robot::wcs_t> Robot::get_wcs_state() const
{
    std::vector<wcs_t> v;
    v.push_back(wcs_t(current_wcs, MAX_WCS, 0));
    for(auto& i : wcs_offsets) {
        v.push_back(i);
    }
    v.push_back(g92_offset);
    v.push_back(tool_offset);
    return v;
}

void Robot::get_current_machine_position(float *pos) const
{
    // get real time current actuator position in mm
    ActuatorCoordinates current_position{
        actuators[X_AXIS]->get_current_position(),
        actuators[Y_AXIS]->get_current_position(),
        actuators[Z_AXIS]->get_current_position()
    };

    // get machine position from the actuator position using FK
    arm_solution->actuator_to_cartesian(current_position, pos);
}

void Robot::print_position(uint8_t subcode, std::string& res, bool ignore_extruders) const
{
    // M114.1 is a new way to do this (similar to how GRBL does it).
    // it returns the realtime position based on the current step position of the actuators.
    // this does require a FK to get a machine position from the actuator position
    // and then invert all the transforms to get a workspace position from machine position
    // M114 just does it the old way uses machine_position and does inverse transforms to get the requested position
    int n = 0;
    char buf[64];
    if(subcode == 0) { // M114 print WCS
        wcs_t pos = mcs2wcs(machine_position);
        n = snprintf(buf, sizeof(buf), "C: X:%1.4f Y:%1.4f Z:%1.4f", from_millimeters(std::get<X_AXIS>(pos)), from_millimeters(std::get<Y_AXIS>(pos)), from_millimeters(std::get<Z_AXIS>(pos)));

    } else if(subcode == 4) {
        // M114.4 print last milestone
        n = snprintf(buf, sizeof(buf), "MP: X:%1.4f Y:%1.4f Z:%1.4f", machine_position[X_AXIS], machine_position[Y_AXIS], machine_position[Z_AXIS]);

    } else if(subcode == 5) {
        // M114.5 print last machine position (which should be the same as M114.1 if axis are not moving and no level compensation)
        // will differ from LMS by the compensation at the current position otherwise
        n = snprintf(buf, sizeof(buf), "CMP: X:%1.4f Y:%1.4f Z:%1.4f", compensated_machine_position[X_AXIS], compensated_machine_position[Y_AXIS], compensated_machine_position[Z_AXIS]);

    } else {
        // get real time positions
        float mpos[3];
        get_current_machine_position(mpos);

        // current_position/mpos includes the compensation transform so we need to get the inverse to get actual position
        if(compensationTransform) compensationTransform(mpos, true); // get inverse compensation transform

        if(subcode == 1) { // M114.1 print realtime WCS
            wcs_t pos = mcs2wcs(mpos);
            n = snprintf(buf, sizeof(buf), "WCS: X:%1.4f Y:%1.4f Z:%1.4f", from_millimeters(std::get<X_AXIS>(pos)), from_millimeters(std::get<Y_AXIS>(pos)), from_millimeters(std::get<Z_AXIS>(pos)));

        } else if(subcode == 2) { // M114.2 print realtime Machine coordinate system
            n = snprintf(buf, sizeof(buf), "MCS: X:%1.4f Y:%1.4f Z:%1.4f", mpos[X_AXIS], mpos[Y_AXIS], mpos[Z_AXIS]);

        } else if(subcode == 3) { // M114.3 print realtime actuator position
            // get real time current actuator position in mm
            ActuatorCoordinates current_position{
                actuators[X_AXIS]->get_current_position(),
                actuators[Y_AXIS]->get_current_position(),
                actuators[Z_AXIS]->get_current_position()
            };
            n = snprintf(buf, sizeof(buf), "APOS: X:%1.4f Y:%1.4f Z:%1.4f", current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS]);
        }
    }

    res.append(buf, n);

#if MAX_ROBOT_ACTUATORS > 3
    // deal with the ABC axis
    for (int i = A_AXIS; i < n_motors; ++i) {
        n = 0;
        if(ignore_extruders && actuators[i]->is_extruder()) continue; // don't show an extruder as that will be E
        if(subcode == 4) { // M114.4 print last milestone
            n = snprintf(buf, sizeof(buf), " %c:%1.4f", 'A' + i - A_AXIS, machine_position[i]);

        } else if(subcode == 2 || subcode == 3) { // M114.2/M114.3 print actuator position which is the same as machine position for ABC
            // current actuator position
            n = snprintf(buf, sizeof(buf), " %c:%1.4f", 'A' + i - A_AXIS, actuators[i]->get_current_position());
        }
        if(n > 0) res.append(buf, n);
    }
#endif
}

// converts current last milestone (machine position without compensation transform) to work coordinate system (inverse transform)
Robot::wcs_t Robot::mcs2wcs(const Robot::wcs_t& pos) const
{
    return std::make_tuple(
               std::get<X_AXIS>(pos) - std::get<X_AXIS>(wcs_offsets[current_wcs]) + std::get<X_AXIS>(g92_offset) - std::get<X_AXIS>(tool_offset),
               std::get<Y_AXIS>(pos) - std::get<Y_AXIS>(wcs_offsets[current_wcs]) + std::get<Y_AXIS>(g92_offset) - std::get<Y_AXIS>(tool_offset),
               std::get<Z_AXIS>(pos) - std::get<Z_AXIS>(wcs_offsets[current_wcs]) + std::get<Z_AXIS>(g92_offset) - std::get<Z_AXIS>(tool_offset)
           );
}

// this does a sanity check that actuator speeds do not exceed steps rate capability
// we will override the actuator max_rate if the combination of max_rate and steps/sec exceeds base_stepping_frequency
void Robot::check_max_actuator_speeds()
{
    for (size_t i = 0; i < n_motors; i++) {
        if(actuators[i]->is_extruder()) continue; //extruders are not included in this check

        float step_freq = actuators[i]->get_max_rate() * actuators[i]->get_steps_per_mm();
        if (step_freq > StepTicker::getInstance()->get_frequency()) {
            actuators[i]->set_max_rate(floorf(StepTicker::getInstance()->get_frequency() / actuators[i]->get_steps_per_mm()));
            printf("WARNING: actuator %d rate exceeds base_stepping_frequency * ..._steps_per_mm: %f, setting to %f\n", i, step_freq, actuators[i]->get_max_rate());
        }
    }
}

bool Robot::handle_dwell(GCode& gcode, OutputStream& os)
{
    // G4 Dwell
    uint32_t delay_ms = 0;
    if (gcode.has_arg('P')) {
        if(is_grbl_mode()) {
            // in grbl mode (and linuxcnc) P is decimal seconds
            float f = gcode.get_arg('P');
            delay_ms = f * 1000.0F;

        } else {
            // in reprap P is milliseconds, they always have to be different!
            delay_ms = gcode.get_int_arg('P');
        }
    }
    if (gcode.has_arg('S')) {
        delay_ms += gcode.get_int_arg('S') * 1000;
    }
    if (delay_ms > 0) {
        // drain queue
        Conveyor::getInstance()->wait_for_idle();
        usleep(delay_ms*1000);
     }

     return true;
}

bool Robot::handle_G10(GCode& gcode, OutputStream& os)
{
    // G10 L2 [L20] Pn Xn Yn Zn set WCS
    if(gcode.has_arg('L') && (gcode.get_int_arg('L') == 2 || gcode.get_int_arg('L') == 20) && gcode.has_arg('P')) {
        size_t n = gcode.get_int_arg('P');
        if(n == 0) n = current_wcs; // set current coordinate system
        else --n;
        if(n < MAX_WCS) {
            float x, y, z;
            std::tie(x, y, z) = wcs_offsets[n];
            if(gcode.get_int_arg('L') == 20) {
                // this makes the current machine position (less compensation transform) the offset
                // get current position in WCS
                wcs_t pos = mcs2wcs(machine_position);

                if(gcode.has_arg('X')) {
                    x -= to_millimeters(gcode.get_arg('X')) - std::get<X_AXIS>(pos);
                }

                if(gcode.has_arg('Y')) {
                    y -= to_millimeters(gcode.get_arg('Y')) - std::get<Y_AXIS>(pos);
                }
                if(gcode.has_arg('Z')) {
                    z -= to_millimeters(gcode.get_arg('Z')) - std::get<Z_AXIS>(pos);
                }

            } else {
                if(absolute_mode) {
                    // the value is the offset from machine zero
                    if(gcode.has_arg('X')) x = to_millimeters(gcode.get_arg('X'));
                    if(gcode.has_arg('Y')) y = to_millimeters(gcode.get_arg('Y'));
                    if(gcode.has_arg('Z')) z = to_millimeters(gcode.get_arg('Z'));
                } else {
                    if(gcode.has_arg('X')) x += to_millimeters(gcode.get_arg('X'));
                    if(gcode.has_arg('Y')) y += to_millimeters(gcode.get_arg('Y'));
                    if(gcode.has_arg('Z')) z += to_millimeters(gcode.get_arg('Z'));
                }
            }
            wcs_offsets[n] = wcs_t(x, y, z);
        }
    }

    return true;
}

bool Robot::handle_G92(GCode& gcode, OutputStream& os)
{
    if(gcode.get_subcode() == 1 || gcode.get_subcode() == 2 || gcode.get_num_args() == 0) {
        // reset G92 offsets to 0
        g92_offset = wcs_t(0, 0, 0);

    } else if(gcode.get_subcode() == 3) {
        // initialize G92 to the specified values, only used for saving it with M500
        float x = 0, y = 0, z = 0;
        if(gcode.has_arg('X')) x = gcode.get_arg('X');
        if(gcode.has_arg('Y')) y = gcode.get_arg('Y');
        if(gcode.has_arg('Z')) z = gcode.get_arg('Z');
        g92_offset = wcs_t(x, y, z);

    } else {
        // standard setting of the g92 offsets, making current WCS position whatever the coordinate arguments are
        float x, y, z;
        std::tie(x, y, z) = g92_offset;
        // get current position in WCS
        wcs_t pos = mcs2wcs(machine_position);

        // adjust g92 offset to make the current wpos == the value requested
        if(gcode.has_arg('X')) {
            x += to_millimeters(gcode.get_arg('X')) - std::get<X_AXIS>(pos);
        }
        if(gcode.has_arg('Y')) {
            y += to_millimeters(gcode.get_arg('Y')) - std::get<Y_AXIS>(pos);
        }
        if(gcode.has_arg('Z')) {
            z += to_millimeters(gcode.get_arg('Z')) - std::get<Z_AXIS>(pos);
        }
        g92_offset = wcs_t(x, y, z);
    }

#if MAX_ROBOT_ACTUATORS > 3
    if(gcode.get_subcode() == 0 && (gcode.has_arg('E') || gcode.get_num_args() == 0)) {
        // reset the E position, legacy for 3d Printers to be reprap compatible
        // find the selected extruder
        int selected_extruder = get_active_extruder();
        if(selected_extruder > 0) {
            float e = gcode.has_arg('E') ? gcode.get_arg('E') : 0;
            machine_position[selected_extruder] = compensated_machine_position[selected_extruder] = e;
            actuators[selected_extruder]->change_last_milestone(get_e_scale_fnc ? e * get_e_scale_fnc() : e);
        }
    }
#endif
    return true;
}

// A GCode has been received
// See if the current Gcode line has some orders for us
bool Robot::handle_gcodes(GCode& gcode, OutputStream& os)
{

    enum MOTION_MODE_T motion_mode = NONE;

    if( gcode.has_g()) {
        switch( gcode.get_code() ) {
            case 0: motion_mode = SEEK;    break;
            case 1: motion_mode = LINEAR;  break;
            case 2: motion_mode = CW_ARC;  break;
            case 3: motion_mode = CCW_ARC; break;
            // case 4: handle_dwell(gcode, os); break;

            // case 10: handle_G10(gcode); break;

            case 17: this->select_plane(X_AXIS, Y_AXIS, Z_AXIS);   break;
            case 18: this->select_plane(X_AXIS, Z_AXIS, Y_AXIS);   break;
            case 19: this->select_plane(Y_AXIS, Z_AXIS, X_AXIS);   break;
            case 20: this->inch_mode = true;   break;
            case 21: this->inch_mode = false;   break;

            case 54: case 55: case 56: case 57: case 58: case 59:
                // select WCS 0-8: G54..G59, G59.1, G59.2, G59.3
                current_wcs = gcode.get_code() - 54;
                if(gcode.get_code() == 59 && gcode.get_subcode() > 0) {
                    current_wcs += gcode.get_subcode();
                    if(current_wcs >= MAX_WCS) current_wcs = MAX_WCS - 1;
                }
                break;

            case 90: this->absolute_mode = true; this->e_absolute_mode = true; break;
            case 91: this->absolute_mode = false; this->e_absolute_mode = false; break;
        }

    } else if( gcode.has_m()) {
        switch( gcode.get_code() ) {
            // case 0: // M0 feed hold, (M0.1 is release feed hold, except we are in feed hold)
            //     if(is_grbl_mode()) THEKERNEL->set_feed_hold(gcode.get_subcode() == 0);
            //     break;

            case 30: // M30 end of program in grbl mode (otherwise it is delete sdcard file)
                if(!is_grbl_mode()) break;
            // fall through to M2
            case 2: // M2 end of program
                current_wcs = 0;
                absolute_mode = true;
                break;
            case 17:
                // TODO
                //THEKERNEL->call_event(ON_ENABLE, (void*)1); // turn all enable pins on
                break;

            case 18: // this allows individual motors to be turned off, no parameters falls through to turn all off
                if(gcode.get_num_args() > 0) {
                    // bitmap of motors to turn off, where bit 1:X, 2:Y, 3:Z, 4:A, 5:B, 6:C
                    uint32_t bm = 0;
                    for (int i = 0; i < n_motors; ++i) {
                        char axis = (i <= Z_AXIS ? 'X' + i : 'A' + (i - 3));
                        if(gcode.has_arg(axis)) bm |= (0x02 << i); // set appropriate bit
                    }
                    // handle E parameter as currently selected extruder ABC
                    if(gcode.has_arg('E')) {
                        // find first selected extruder
                        int i = get_active_extruder();
                        if(i > 0) {
                            bm |= (0x02 << i); // set appropriate bit
                        }
                    }

                    Conveyor::getInstance()->wait_for_idle();
                    //THEKERNEL->call_event(ON_ENABLE, (void *)bm); // TODO
                    break;
                }
            // fall through
            case 84:
                Conveyor::getInstance()->wait_for_idle();
                //THEKERNEL->call_event(ON_ENABLE, nullptr); // TODO turn all enable pins off
                break;

            case 82: e_absolute_mode = true; break;
            case 83: e_absolute_mode = false; break;

            case 92: // M92 - set steps per mm
                for (int i = 0; i < n_motors; ++i) {
                    if(actuators[i]->is_extruder()) continue; //extruders handle this themselves
                    char axis = (i <= Z_AXIS ? 'X' + i : 'A' + (i - A_AXIS));
                    if(gcode.has_arg(axis)) {
                        actuators[i]->change_steps_per_mm(this->to_millimeters(gcode.get_arg(axis)));
                    }
                    os.printf("%c:%f ", axis, actuators[i]->get_steps_per_mm());
                }
                os.set_append_nl();
                check_max_actuator_speeds();
                return true;

            case 114: {
                std::string buf;
                print_position(gcode.get_subcode(), buf, true); // ignore extruders as they will print E themselves
                os.set_prepend_ok();
                os.puts(buf.c_str());
                return true;;
            }

            case 120: // push state
                push_state();
                break;

            case 121: // pop state
                pop_state();
                break;

            case 203: // M203 Set maximum feedrates in mm/sec, M203.1 set maximum actuator feedrates
                if(gcode.get_num_args() == 0) {
                    for (size_t i = X_AXIS; i <= Z_AXIS; i++) {
                        os.printf(" %c: %g ", 'X' + i, gcode.get_subcode() == 0 ? this->max_speeds[i] : actuators[i]->get_max_rate());
                    }
                    if(gcode.get_subcode() == 1) {
                        for (size_t i = A_AXIS; i < n_motors; i++) {
                            if(actuators[i]->is_extruder()) continue; //extruders handle this themselves
                            os.printf(" %c: %g ", 'A' + i - A_AXIS, actuators[i]->get_max_rate());
                        }
                    }

                    os.set_append_nl();

                } else {
                    for (size_t i = X_AXIS; i <= Z_AXIS; i++) {
                        if (gcode.has_arg('X' + i)) {
                            float v = gcode.get_arg('X' + i);
                            if(gcode.get_subcode() == 0) this->max_speeds[i] = v;
                            else if(gcode.get_subcode() == 1) actuators[i]->set_max_rate(v);
                        }
                    }

                    if(gcode.get_subcode() == 1) {
                        // ABC axis only handle actuator max speeds
                        for (size_t i = A_AXIS; i < n_motors; i++) {
                            if(actuators[i]->is_extruder()) continue; //extruders handle this themselves
                            int c = 'A' + i - A_AXIS;
                            if(gcode.has_arg(c)) {
                                float v = gcode.get_arg(c);
                                actuators[i]->set_max_rate(v);
                            }
                        }
                    }


                    // this format is deprecated
                    if(gcode.get_subcode() == 0 && (gcode.has_arg('A') || gcode.has_arg('B') || gcode.has_arg('C'))) {
                        os.printf("NOTE this format is deprecated, Use M203.1 instead\n");
                        for (size_t i = X_AXIS; i <= Z_AXIS; i++) {
                            if (gcode.has_arg('A' + i)) {
                                float v = gcode.get_arg('A' + i);
                                actuators[i]->set_max_rate(v);
                            }
                        }
                    }

                    if(gcode.get_subcode() == 1) check_max_actuator_speeds();
                }
                break;

            case 204: // M204 Snnn - set default acceleration to nnn, Xnnn Ynnn Znnn sets axis specific acceleration
                if (gcode.has_arg('S')) {
                    float acc = gcode.get_arg('S'); // mm/s^2
                    // enforce minimum
                    if (acc < 1.0F) acc = 1.0F;
                    this->default_acceleration = acc;
                }
                for (int i = 0; i < n_motors; ++i) {
                    if(actuators[i]->is_extruder()) continue; //extruders handle this themselves
                    char axis = (i <= Z_AXIS ? 'X' + i : 'A' + (i - A_AXIS));
                    if(gcode.has_arg(axis)) {
                        float acc = gcode.get_arg(axis); // mm/s^2
                        // enforce positive
                        if (acc <= 0.0F) acc = NAN;
                        actuators[i]->set_acceleration(acc);
                    }
                }
                break;

            case 205: // M205 Xnnn - set junction deviation, Z - set Z junction deviation, Snnn - Set minimum planner speed
                if (gcode.has_arg('X')) {
                    float jd = gcode.get_arg('X');
                    // enforce minimum
                    if (jd < 0.0F)
                        jd = 0.0F;
                    Planner::getInstance()->xy_junction_deviation = jd;
                }
                if (gcode.has_arg('Z')) {
                    float jd = gcode.get_arg('Z');
                    // enforce minimum, -1 disables it and uses regular junction deviation
                    if (jd <= -1.0F)
                        jd = NAN;
                    Planner::getInstance()->z_junction_deviation = jd;
                }
                if (gcode.has_arg('S')) {
                    float mps = gcode.get_arg('S');
                    // enforce minimum
                    if (mps < 0.0F)
                        mps = 0.0F;
                    Planner::getInstance()->minimum_planner_speed = mps;
                }
                break;

            case 220: // M220 - speed override percentage
                if (gcode.has_arg('S')) {
                    float factor = gcode.get_arg('S');
                    // enforce minimum 10% speed
                    if (factor < 10.0F)
                        factor = 10.0F;
                    // enforce maximum 10x speed
                    if (factor > 1000.0F)
                        factor = 1000.0F;

                    seconds_per_minute = 6000.0F / factor;
                } else {
                    os.printf("Speed factor at %6.2f %%\n", 6000.0F / seconds_per_minute);
                }
                break;

            case 400: // wait until all moves are done up to this point
                Conveyor::getInstance()->wait_for_idle();
                break;

            case 500: // M500 saves some volatile settings to config override file
            case 503: { // M503 just prints the settings
                os.printf(";Steps per unit:\nM92 ");
                for (int i = 0; i < n_motors; ++i) {
                    if(actuators[i]->is_extruder()) continue; //extruders handle this themselves
                    char axis = (i <= Z_AXIS ? 'X' + i : 'A' + (i - A_AXIS));
                    os.printf("%c%1.5f ", axis, actuators[i]->get_steps_per_mm());
                }
                os.printf("\n");

                // only print if not NAN
                os.printf(";Acceleration mm/sec^2:\nM204 S%1.5f ", default_acceleration);
                for (int i = 0; i < n_motors; ++i) {
                    if(actuators[i]->is_extruder()) continue; // extruders handle this themselves
                    char axis = (i <= Z_AXIS ? 'X' + i : 'A' + (i - A_AXIS));
                    if(!isnan(actuators[i]->get_acceleration())) os.printf("%c%1.5f ", axis, actuators[i]->get_acceleration());
                }
                os.printf("\n");

                os.printf(";X- Junction Deviation, Z- Z junction deviation, S - Minimum Planner speed mm/sec:\nM205 X%1.5f Z%1.5f S%1.5f\n", Planner::getInstance()->xy_junction_deviation, isnan(Planner::getInstance()->z_junction_deviation) ? -1 : Planner::getInstance()->z_junction_deviation, Planner::getInstance()->minimum_planner_speed);

                os.printf(";Max cartesian feedrates in mm/sec:\nM203 X%1.5f Y%1.5f Z%1.5f\n", this->max_speeds[X_AXIS], this->max_speeds[Y_AXIS], this->max_speeds[Z_AXIS]);

                os.printf(";Max actuator feedrates in mm/sec:\nM203.1 ");
                for (int i = 0; i < n_motors; ++i) {
                    if(actuators[i]->is_extruder()) continue; // extruders handle this themselves
                    char axis = (i <= Z_AXIS ? 'X' + i : 'A' + (i - A_AXIS));
                    os.printf("%c%1.5f ", axis, actuators[i]->get_max_rate());
                }
                os.printf("\n");

                // get or save any arm solution specific optional values
                BaseSolution::arm_options_t options;
                if(arm_solution->get_optional(options) && !options.empty()) {
                    os.printf(";Optional arm solution specific settings:\nM665");
                    for(auto &i : options) {
                        os.printf(" %c%1.4f", i.first, i.second);
                    }
                    os.printf("\n");
                }

                // save wcs_offsets and current_wcs
                // TODO this may need to be done whenever they change to be compliant
                os.printf(";WCS settings\n");
                os.printf("%s\n", wcs2gcode(current_wcs).c_str());
                int n = 1;
                for(auto &i : wcs_offsets) {
                    if(i != wcs_t(0, 0, 0)) {
                        float x, y, z;
                        std::tie(x, y, z) = i;
                        os.printf("G10 L2 P%d X%f Y%f Z%f ; %s\n", n, x, y, z, wcs2gcode(n - 1).c_str());
                    }
                    ++n;
                }
                if(save_g92) {
                    // linuxcnc saves G92, so we do too if configured, default is to not save to maintain backward compatibility
                    // also it needs to be used to set Z0 on rotary deltas as M206/306 can't be used, so saving it is necessary in that case
                    if(g92_offset != wcs_t(0, 0, 0)) {
                        float x, y, z;
                        std::tie(x, y, z) = g92_offset;
                        os.printf("G92.3 X%f Y%f Z%f\n", x, y, z); // sets G92 to the specified values
                    }
                }
            }
            break;

            case 665: { // M665 set optional arm solution variables based on arm solution.
                // the parameter args could be any letter each arm solution only accepts certain ones
                BaseSolution::arm_options_t options = gcode.get_args();
                options.erase('S'); // don't include the S
                options.erase('U'); // don't include the U
                if(options.size() > 0) {
                    // set the specified options
                    arm_solution->set_optional(options);
                }
                options.clear();
                if(arm_solution->get_optional(options)) {
                    // foreach optional value
                    for(auto &i : options) {
                        // print all current values of supported options
                        os.printf("%c: %8.4f ", i.first, i.second);
                        os.set_append_nl();
                    }
                }

                if(gcode.has_arg('S')) { // set delta segments per second, not saved by M500
                    this->delta_segments_per_second = gcode.get_arg('S');
                    os.printf("Delta segments set to %8.4f segs/sec\n", this->delta_segments_per_second);

                } else if(gcode.has_arg('U')) { // or set mm_per_line_segment, not saved by M500
                    this->mm_per_line_segment = gcode.get_arg('U');
                    this->delta_segments_per_second = 0;
                    os.printf("mm per line segment set to %8.4f\n", this->mm_per_line_segment);
                }

                break;
            }
        }
    }

    if( motion_mode != NONE) {
        is_g123 = motion_mode != SEEK;
        process_move(gcode, motion_mode);

    } else {
        is_g123 = false;
    }

    next_command_is_MCS = false; // must be on same line as G0 or G1

    return true;
}

int Robot::get_active_extruder() const
{
    for (int i = E_AXIS; i < n_motors; ++i) {
        // find first selected extruder
        if(actuators[i]->is_extruder() && actuators[i]->is_selected()) return i;
    }
    return 0;
}

// process a G0/G1/G2/G3
void Robot::process_move(GCode& gcode, enum MOTION_MODE_T motion_mode)
{
    // we have a G0/G1/G2/G3 so extract parameters and apply offsets to get machine coordinate target
    // get XYZ and one E (which goes to the selected extruder)
    float param[4] {NAN, NAN, NAN, NAN};

    // process primary axis
    for(int i = X_AXIS; i <= Z_AXIS; ++i) {
        char letter = 'X' + i;
        if( gcode.has_arg(letter) ) {
            param[i] = this->to_millimeters(gcode.get_arg(letter));
        }
    }

    float offset[3] {0, 0, 0};
    for(char letter = 'I'; letter <= 'K'; letter++) {
        if( gcode.has_arg(letter) ) {
            offset[letter - 'I'] = this->to_millimeters(gcode.get_arg(letter));
        }
    }

    // calculate target in machine coordinates (less compensation transform which needs to be done after segmentation)
    float target[n_motors];
    memcpy(target, machine_position, n_motors * sizeof(float));

    if(!next_command_is_MCS) {
        if(this->absolute_mode) {
            // apply wcs offsets and g92 offset and tool offset
            if(!isnan(param[X_AXIS])) {
                target[X_AXIS] = param[X_AXIS] + std::get<X_AXIS>(wcs_offsets[current_wcs]) - std::get<X_AXIS>(g92_offset) + std::get<X_AXIS>(tool_offset);
            }

            if(!isnan(param[Y_AXIS])) {
                target[Y_AXIS] = param[Y_AXIS] + std::get<Y_AXIS>(wcs_offsets[current_wcs]) - std::get<Y_AXIS>(g92_offset) + std::get<Y_AXIS>(tool_offset);
            }

            if(!isnan(param[Z_AXIS])) {
                target[Z_AXIS] = param[Z_AXIS] + std::get<Z_AXIS>(wcs_offsets[current_wcs]) - std::get<Z_AXIS>(g92_offset) + std::get<Z_AXIS>(tool_offset);
            }

        } else {
            // they are deltas from the machine_position if specified
            for(int i = X_AXIS; i <= Z_AXIS; ++i) {
                if(!isnan(param[i])) target[i] = param[i] + machine_position[i];
            }
        }

    } else {
        // already in machine coordinates, we do not add wcs or tool offset for that
        for(int i = X_AXIS; i <= Z_AXIS; ++i) {
            if(!isnan(param[i])) target[i] = param[i];
        }
    }

    float delta_e = NAN;

#if MAX_ROBOT_ACTUATORS > 3
    // process extruder parameters, for active extruder only (only one active extruder at a time)
    int selected_extruder = 0;
    if(gcode.has_arg('E')) {
        selected_extruder = get_active_extruder();
        param[E_AXIS] = gcode.get_arg('E');
    }

    // do E for the selected extruder
    if(selected_extruder > 0 && !isnan(param[E_AXIS])) {
        if(this->e_absolute_mode) {
            target[selected_extruder] = param[E_AXIS];
            delta_e = target[selected_extruder] - machine_position[selected_extruder];
        } else {
            delta_e = param[E_AXIS];
            target[selected_extruder] = delta_e + machine_position[selected_extruder];
        }
    }

    // process ABC axis, this is mutually exclusive to using E for an extruder, so if E is used and A then the results are undefined
    for (int i = A_AXIS; i < n_motors; ++i) {
        char letter = 'A' + i - A_AXIS;
        if(gcode.has_arg(letter)) {
            float p = gcode.get_arg(letter);
            if(this->absolute_mode) {
                target[i] = p;
            } else {
                target[i] = p + machine_position[i];
            }
        }
    }
#endif

    if( gcode.has_arg('F') ) {
        if( motion_mode == SEEK )
            this->seek_rate = this->to_millimeters( gcode.get_arg('F') );
        else
            this->feed_rate = this->to_millimeters( gcode.get_arg('F') );
    }

    // S is modal When specified on a G0/1/2/3 command
    if(gcode.has_arg('S')) s_value = gcode.get_arg('S');

    bool moved = false;

    // Perform any physical actions
    switch(motion_mode) {
        case NONE: break;

        case SEEK:
            moved = this->append_line(gcode, target, this->seek_rate / seconds_per_minute, delta_e );
            break;

        case LINEAR:
            moved = this->append_line(gcode, target, this->feed_rate / seconds_per_minute, delta_e );
            break;

        case CW_ARC:
        case CCW_ARC:
            // Note arcs are not currently supported by extruder based machines, as 3D slicers do not use arcs (G2/G3)
            moved = this->compute_arc(gcode, offset, target, motion_mode);
            break;
    }

    if(moved) {
        // set machine_position to the calculated target
        memcpy(machine_position, target, n_motors * sizeof(float));
    }
}

// reset the machine position for all axis. Used for homing.
// after homing we supply the cartesian coordinates that the head is at when homed,
// however for Z this is the compensated machine position (if enabled)
// So we need to apply the inverse compensation transform to the supplied coordinates to get the correct machine position
// this will make the results from M114 and ? consistent after homing.
// This works for cases where the Z endstop is fixed on the Z actuator and is the same regardless of where XY are.
void Robot::reset_axis_position(float x, float y, float z)
{
    // set both the same initially
    compensated_machine_position[X_AXIS] = machine_position[X_AXIS] = x;
    compensated_machine_position[Y_AXIS] = machine_position[Y_AXIS] = y;
    compensated_machine_position[Z_AXIS] = machine_position[Z_AXIS] = z;

    if(compensationTransform) {
        // apply inverse transform to get machine_position
        compensationTransform(machine_position, true);
    }

    // now set the actuator positions based on the supplied compensated position
    ActuatorCoordinates actuator_pos;
    arm_solution->cartesian_to_actuator(this->compensated_machine_position, actuator_pos);
    for (size_t i = X_AXIS; i <= Z_AXIS; i++)
        actuators[i]->change_last_milestone(actuator_pos[i]);
}

// Reset the position for an axis (used in homing, and to reset extruder after suspend)
void Robot::reset_axis_position(float position, int axis)
{
    compensated_machine_position[axis] = position;
    if(axis <= Z_AXIS) {
        reset_axis_position(compensated_machine_position[X_AXIS], compensated_machine_position[Y_AXIS], compensated_machine_position[Z_AXIS]);

#if MAX_ROBOT_ACTUATORS > 3
    } else if(axis < n_motors) {
        // ABC and/or extruders need to be set as there is no arm solution for them
        machine_position[axis] = compensated_machine_position[axis] = position;
        actuators[axis]->change_last_milestone(machine_position[axis]);
#endif
    }
}

// similar to reset_axis_position but directly sets the actuator positions in actuators units (eg mm for cartesian, degrees for rotary delta)
// then sets the axis positions to match. currently only called from Endstops.cpp and RotaryDeltaCalibration.cpp
void Robot::reset_actuator_position(const ActuatorCoordinates &ac)
{
    for (size_t i = X_AXIS; i <= Z_AXIS; i++) {
        if(!isnan(ac[i])) actuators[i]->change_last_milestone(ac[i]);
    }

    // now correct axis positions then recorrect actuator to account for rounding
    reset_position_from_current_actuator_position();
}

// Use FK to find out where actuator is and reset to match
// TODO maybe we should only reset axis that are being homed unless this is due to a ON_HALT
void Robot::reset_position_from_current_actuator_position()
{
    ActuatorCoordinates actuator_pos;
    for (size_t i = X_AXIS; i < n_motors; i++) {
        // NOTE actuator::current_position is curently NOT the same as actuator::machine_position after an abrupt abort
        actuator_pos[i] = actuators[i]->get_current_position();
    }

    // discover machine position from where actuators actually are
    arm_solution->actuator_to_cartesian(actuator_pos, compensated_machine_position);
    memcpy(machine_position, compensated_machine_position, sizeof machine_position);

    // compensated_machine_position includes the compensation transform so we need to get the inverse to get actual machine_position
    if(compensationTransform) compensationTransform(machine_position, true); // get inverse compensation transform

    // now reset actuator::machine_position, NOTE this may lose a little precision as FK is not always entirely accurate.
    // NOTE This is required to sync the machine position with the actuator position, we do a somewhat redundant cartesian_to_actuator() call
    // to get everything in perfect sync.
    arm_solution->cartesian_to_actuator(compensated_machine_position, actuator_pos);
    for (size_t i = X_AXIS; i <= Z_AXIS; i++) {
        actuators[i]->change_last_milestone(actuator_pos[i]);
    }

    // Handle extruders and/or ABC axis
#if MAX_ROBOT_ACTUATORS > 3
    for (int i = A_AXIS; i < n_motors; i++) {
        // ABC and/or extruders just need to set machine_position and compensated_machine_position
        float ap = actuator_pos[i];
        if(actuators[i]->is_extruder() && get_e_scale_fnc) ap /= get_e_scale_fnc(); // inverse E scale if there is one and this is an extruder
        machine_position[i] = compensated_machine_position[i] = ap;
        actuators[i]->change_last_milestone(actuator_pos[i]); // this updates the last_milestone in the actuator
    }
#endif
}

// Convert target (in machine coordinates) to machine_position, then convert to actuator position and append this to the planner
// target is in machine coordinates without the compensation transform, however we save a compensated_machine_position that includes
// all transforms and is what we actually convert to actuator positions
bool Robot::append_milestone(const float target[], float rate_mm_s)
{
    float deltas[n_motors];
    float transformed_target[n_motors]; // adjust target for bed compensation
    float unit_vec[N_PRIMARY_AXIS];

    // unity transform by default
    memcpy(transformed_target, target, n_motors * sizeof(float));

    // check function pointer and call if set to transform the target to compensate for bed
    if(compensationTransform) {
        // some compensation strategies can transform XYZ, some just change Z
        compensationTransform(transformed_target, false);
    }

    bool move = false;
    float sos = 0; // sum of squares for just primary axis (XYZ usually)

    // find distance moved by each axis, use transformed target from the current compensated machine position
    for (size_t i = 0; i < n_motors; i++) {
        deltas[i] = transformed_target[i] - compensated_machine_position[i];
        if(deltas[i] == 0) continue;
        // at least one non zero delta
        move = true;
        if(i < N_PRIMARY_AXIS) {
            sos += powf(deltas[i], 2);
        }
    }

    // nothing moved
    if(!move) return false;

    // see if this is a primary axis move or not
    bool auxilliary_move = true;
    for (int i = 0; i < N_PRIMARY_AXIS; ++i) {
        if(deltas[i] != 0) {
            auxilliary_move = false;
            break;
        }
    }

    // total movement, use XYZ if a primary axis otherwise we calculate distance for E after scaling to mm
    float distance = auxilliary_move ? 0 : sqrtf(sos);

    // it is unlikely but we need to protect against divide by zero, so ignore insanely small moves here
    // as the last milestone won't be updated we do not actually lose any moves as they will be accounted for in the next move
    if(!auxilliary_move && distance < 0.00001F) return false;


    if(!auxilliary_move) {
        for (size_t i = X_AXIS; i < N_PRIMARY_AXIS; i++) {
            // find distance unit vector for primary axis only
            unit_vec[i] = deltas[i] / distance;

            // Do not move faster than the configured cartesian limits for XYZ
            if ( i <= Z_AXIS && max_speeds[i] > 0 ) {
                float axis_speed = fabsf(unit_vec[i] * rate_mm_s);

                if (axis_speed > max_speeds[i])
                    rate_mm_s *= ( max_speeds[i] / axis_speed );
            }
        }
    }

    // find actuator position given the machine position, use actual adjusted target
    ActuatorCoordinates actuator_pos;
    if(!disable_arm_solution) {
        arm_solution->cartesian_to_actuator( transformed_target, actuator_pos );

    } else {
        // basically the same as cartesian, would be used for special homing situations like for scara
        for (size_t i = X_AXIS; i <= Z_AXIS; i++) {
            actuator_pos[i] = transformed_target[i];
        }
    }

#if MAX_ROBOT_ACTUATORS > 3
    sos = 0;
    // for the extruders just copy the position, and possibly scale it from mm³ to mm
    for (size_t i = E_AXIS; i < n_motors; i++) {
        actuator_pos[i] = transformed_target[i];
        if(actuators[i]->is_extruder() && get_e_scale_fnc) {
            // NOTE this relies on the fact only one extruder is active at a time
            // scale for volumetric or flow rate
            // TODO is this correct? scaling the absolute target? what if the scale changes?
            // for volumetric it basically converts mm³ to mm, but what about flow rate?
            actuator_pos[i] *= get_e_scale_fnc();
        }
        if(auxilliary_move) {
            // for E only moves we need to use the scaled E to calculate the distance
            sos += powf(actuator_pos[i] - actuators[i]->get_last_milestone(), 2);
        }
    }
    if(auxilliary_move) {
        distance = sqrtf(sos); // distance in mm of the e move
        if(distance < 0.00001F) return false;
    }
#endif

    // use default acceleration to start with
    float acceleration = default_acceleration;

    float isecs = rate_mm_s / distance;

    // check per-actuator speed limits
    for (size_t actuator = 0; actuator < n_motors; actuator++) {
        float d = fabsf(actuator_pos[actuator] - actuators[actuator]->get_last_milestone());
        if(d == 0 || !actuators[actuator]->is_selected()) continue; // no movement for this actuator

        float actuator_rate = d * isecs;
        if (actuator_rate > actuators[actuator]->get_max_rate()) {
            rate_mm_s *= (actuators[actuator]->get_max_rate() / actuator_rate);
            isecs = rate_mm_s / distance;
        }

        // adjust acceleration to lowest found, for now just primary axis unless it is an auxiliary move
        // TODO we may need to do all of them, check E won't limit XYZ.. it does on long E moves, but not checking it could exceed the E acceleration.
        if(auxilliary_move || actuator < N_PRIMARY_AXIS) {
            float ma =  actuators[actuator]->get_acceleration(); // in mm/sec²
            if(!isnan(ma)) {  // if axis does not have acceleration set then it uses the default_acceleration
                float ca = fabsf((d / distance) * acceleration);
                if (ca > ma) {
                    acceleration *= ( ma / ca );
                }
            }
        }
    }

    // if we are in feed hold wait here until it is released, this means that even segmented lines will pause
    // TODO
    // while(THEKERNEL->get_feed_hold()) {
    //     usleep(100000);
    //     // if we also got a HALT then break out of this
    //     if(is_halted) return false;
    // }

    // Append the block to the planner
    // NOTE that distance here should be either the distance travelled by the XYZ axis, or the E mm travel if a solo E move
    // NOTE this call will bock until there is room in the block queue, on_idle will continue to be called
    if(Planner::getInstance()->append_block( actuator_pos, n_motors, rate_mm_s, distance, auxilliary_move ? nullptr : unit_vec, acceleration, s_value, is_g123)) {
        // this is the new compensated machine position
        memcpy(this->compensated_machine_position, transformed_target, n_motors * sizeof(float));
        return true;
    }

    // no actual move
    return false;
}

// Used to plan a single move used by things like endstops when homing, zprobe, extruder firmware retracts etc.
bool Robot::delta_move(const float *delta, float rate_mm_s, uint8_t naxis)
{
    if(is_halted) return false;

    // catch negative or zero feed rates
    if(rate_mm_s <= 0.0F) {
        return false;
    }

    // get the absolute target position, default is current machine_position
    float target[n_motors];
    memcpy(target, machine_position, n_motors * sizeof(float));

    // add in the deltas to get new target
    for (int i = 0; i < naxis; i++) {
        target[i] += delta[i];
    }

    // submit for planning and if moved update machine_position
    if(append_milestone(target, rate_mm_s)) {
        memcpy(machine_position, target, n_motors * sizeof(float));
        return true;
    }

    return false;
}

// Append a move to the queue ( cutting it into segments if needed )
bool Robot::append_line(GCode& gcode, const float target[], float rate_mm_s, float delta_e)
{
    // catch negative or zero feed rates and return the same error as GRBL does
    if(rate_mm_s <= 0.0F) {
        gcode.set_error(rate_mm_s == 0 ? "Undefined feed rate" : "feed rate < 0");
        return false;
    }

    // Find out the distance for this move in XYZ in MCS
    float millimeters_of_travel = sqrtf(powf( target[X_AXIS] - machine_position[X_AXIS], 2 ) +  powf( target[Y_AXIS] - machine_position[Y_AXIS], 2 ) +  powf( target[Z_AXIS] - machine_position[Z_AXIS], 2 ));

    if(millimeters_of_travel < 0.00001F) {
        // we have no movement in XYZ, probably E only extrude or retract
        return this->append_milestone(target, rate_mm_s);
    }

    /*
        For extruders, we need to do some extra work to limit the volumetric rate if specified...
        If using volumetric limts we need to be using volumetric extrusion for this to work as Ennn needs to be in mm³ not mm
        We ask Extruder to do all the work but we need to pass in the relevant data.
        NOTE we need to do this before we segment the line (for deltas)
    */
    if(!isnan(delta_e) && gcode.has_g() && gcode.get_code() == 1) {
        // TODO
        //float data[2] = {delta_e, rate_mm_s / millimeters_of_travel};
        // if(PublicData::set_value(extruder_key, target_key, data)) {
        //     rate_mm_s *= data[1]; // adjust the feedrate
        // }
    }

    // We cut the line into smaller segments. This is only needed on a cartesian robot for zgrid, but always necessary for robots with rotational axes like Deltas.
    // In delta robots either mm_per_line_segment can be used OR delta_segments_per_second
    // The latter is more efficient and avoids splitting fast long lines into very small segments, like initial z move to 0, it is what Johanns Marlin delta port does
    uint16_t segments;

    if(this->disable_segmentation || (!segment_z_moves && !gcode.has_arg('X') && !gcode.has_arg('Y'))) {
        segments = 1;

    } else if(this->delta_segments_per_second > 1.0F) {
        // enabled if set to something > 1, it is set to 0.0 by default
        // segment based on current speed and requested segments per second
        // the faster the travel speed the fewer segments needed
        // NOTE rate is mm/sec and we take into account any speed override
        float seconds = millimeters_of_travel / rate_mm_s;
        segments = std::max(1.0F, ceilf(this->delta_segments_per_second * seconds));
        // TODO if we are only moving in Z on a delta we don't really need to segment at all

    } else {
        if(this->mm_per_line_segment == 0.0F) {
            segments = 1; // don't split it up
        } else {
            segments = ceilf( millimeters_of_travel / this->mm_per_line_segment);
        }
    }

    bool moved = false;
    if (segments > 1) {
        // A vector to keep track of the endpoint of each segment
        float segment_delta[n_motors];
        float segment_end[n_motors];
        memcpy(segment_end, machine_position, n_motors * sizeof(float));

        // How far do we move each segment?
        for (int i = 0; i < n_motors; i++)
            segment_delta[i] = (target[i] - machine_position[i]) / segments;

        // segment 0 is already done - it's the end point of the previous move so we start at segment 1
        // We always add another point after this loop so we stop at segments-1, ie i < segments
        for (int i = 1; i < segments; i++) {
            if(is_halted) return false; // don't queue any more segments
            for (int j = 0; j < n_motors; j++)
                segment_end[j] += segment_delta[j];

            // Append the end of this segment to the queue
            // this can block waiting for free block queue or if in feed hold
            bool b = this->append_milestone(segment_end, rate_mm_s);
            moved = moved || b;
        }
    }

    // Append the end of this full move to the queue
    if(this->append_milestone(target, rate_mm_s)) moved = true;

    this->next_command_is_MCS = false; // always reset this

    return moved;
}


// Append an arc to the queue ( cutting it into segments as needed )
// TODO does not support any E parameters so cannot be used for 3D printing.
bool Robot::append_arc(GCode&  gcode, const float target[], const float offset[], float radius, bool is_clockwise )
{
    float rate_mm_s = this->feed_rate / seconds_per_minute;
    // catch negative or zero feed rates and return the same error as GRBL does
    if(rate_mm_s <= 0.0F) {
        gcode.set_error(rate_mm_s == 0 ? "Undefined feed rate" : "feed rate < 0");
        return false;
    }

    // Scary math
    float center_axis0 = this->machine_position[this->plane_axis_0] + offset[this->plane_axis_0];
    float center_axis1 = this->machine_position[this->plane_axis_1] + offset[this->plane_axis_1];
    float linear_travel = target[this->plane_axis_2] - this->machine_position[this->plane_axis_2];
    float r_axis0 = -offset[this->plane_axis_0]; // Radius vector from center to current location
    float r_axis1 = -offset[this->plane_axis_1];
    float rt_axis0 = target[this->plane_axis_0] - center_axis0;
    float rt_axis1 = target[this->plane_axis_1] - center_axis1;

    // Patch from GRBL Firmware - Christoph Baumann 04072015
    // CCW angle between position and target from circle center. Only one atan2() trig computation required.
    float angular_travel = atan2f(r_axis0 * rt_axis1 - r_axis1 * rt_axis0, r_axis0 * rt_axis0 + r_axis1 * rt_axis1);
    if (is_clockwise) { // Correct atan2 output per direction
        if (angular_travel >= -ARC_ANGULAR_TRAVEL_EPSILON) { angular_travel -= (2 * PI); }
    } else {
        if (angular_travel <= ARC_ANGULAR_TRAVEL_EPSILON) { angular_travel += (2 * PI); }
    }

    // Find the distance for this gcode
    float millimeters_of_travel = hypotf(angular_travel * radius, fabsf(linear_travel));

    // We don't care about non-XYZ moves ( for example the extruder produces some of those )
    if( millimeters_of_travel < 0.00001F ) {
        return false;
    }

    // limit segments by maximum arc error
    float arc_segment = this->mm_per_arc_segment;
    if ((this->mm_max_arc_error > 0) && (2 * radius > this->mm_max_arc_error)) {
        float min_err_segment = 2 * sqrtf((this->mm_max_arc_error * (2 * radius - this->mm_max_arc_error)));
        if (this->mm_per_arc_segment < min_err_segment) {
            arc_segment = min_err_segment;
        }
    }
    // Figure out how many segments for this gcode
    // TODO for deltas we need to make sure we are at least as many segments as requested, also if mm_per_line_segment is set we need to use the
    uint16_t segments = ceilf(millimeters_of_travel / arc_segment);

    //printf("Radius %f - Segment Length %f - Number of Segments %d\r\n",radius,arc_segment,segments);  // Testing Purposes ONLY
    float theta_per_segment = angular_travel / segments;
    float linear_per_segment = linear_travel / segments;

    /* Vector rotation by transformation matrix: r is the original vector, r_T is the rotated vector,
    and phi is the angle of rotation. Based on the solution approach by Jens Geisler.
    r_T = [cos(phi) -sin(phi);
    sin(phi) cos(phi] * r ;
    For arc generation, the center of the circle is the axis of rotation and the radius vector is
    defined from the circle center to the initial position. Each line segment is formed by successive
    vector rotations. This requires only two cos() and sin() computations to form the rotation
    matrix for the duration of the entire arc. Error may accumulate from numerical round-off, since
    all float numbers are single precision on the Arduino. (True float precision will not have
    round off issues for CNC applications.) Single precision error can accumulate to be greater than
    tool precision in some cases. Therefore, arc path correction is implemented.

    Small angle approximation may be used to reduce computation overhead further. This approximation
    holds for everything, but very small circles and large mm_per_arc_segment values. In other words,
    theta_per_segment would need to be greater than 0.1 rad and N_ARC_CORRECTION would need to be large
    to cause an appreciable drift error. N_ARC_CORRECTION~=25 is more than small enough to correct for
    numerical drift error. N_ARC_CORRECTION may be on the order a hundred(s) before error becomes an
    issue for CNC machines with the single precision Arduino calculations.
    This approximation also allows mc_arc to immediately insert a line segment into the planner
    without the initial overhead of computing cos() or sin(). By the time the arc needs to be applied
    a correction, the planner should have caught up to the lag caused by the initial mc_arc overhead.
    This is important when there are successive arc motions.
    */
    // Vector rotation matrix values
    float cos_T = 1 - 0.5F * theta_per_segment * theta_per_segment; // Small angle approximation
    float sin_T = theta_per_segment;

    // TODO we need to handle the ABC axis here by segmenting them
    float arc_target[n_motors];
    float sin_Ti;
    float cos_Ti;
    float r_axisi;
    uint16_t i;
    int8_t count = 0;

    // init array for all axis
    memcpy(arc_target, machine_position, n_motors * sizeof(float));

    // Initialize the linear axis
    arc_target[this->plane_axis_2] = this->machine_position[this->plane_axis_2];

    bool moved = false;
    for (i = 1; i < segments; i++) { // Increment (segments-1)
        if(is_halted) return false; // don't queue any more segments

        if (count < this->arc_correction ) {
            // Apply vector rotation matrix
            r_axisi = r_axis0 * sin_T + r_axis1 * cos_T;
            r_axis0 = r_axis0 * cos_T - r_axis1 * sin_T;
            r_axis1 = r_axisi;
            count++;
        } else {
            // Arc correction to radius vector. Computed only every N_ARC_CORRECTION increments.
            // Compute exact location by applying transformation matrix from initial radius vector(=-offset).
            cos_Ti = cosf(i * theta_per_segment);
            sin_Ti = sinf(i * theta_per_segment);
            r_axis0 = -offset[this->plane_axis_0] * cos_Ti + offset[this->plane_axis_1] * sin_Ti;
            r_axis1 = -offset[this->plane_axis_0] * sin_Ti - offset[this->plane_axis_1] * cos_Ti;
            count = 0;
        }

        // Update arc_target location
        arc_target[this->plane_axis_0] = center_axis0 + r_axis0;
        arc_target[this->plane_axis_1] = center_axis1 + r_axis1;
        arc_target[this->plane_axis_2] += linear_per_segment;

        // Append this segment to the queue
        bool b = this->append_milestone(arc_target, rate_mm_s);
        moved = moved || b;
    }

    // Ensure last segment arrives at target location.
    if(this->append_milestone(target, rate_mm_s)) moved = true;

    return moved;
}

// Do the math for an arc and add it to the queue
bool Robot::compute_arc(GCode&  gcode, const float offset[], const float target[], enum MOTION_MODE_T motion_mode)
{

    // Find the radius
    float radius = hypotf(offset[this->plane_axis_0], offset[this->plane_axis_1]);

    // Set clockwise/counter-clockwise sign for mc_arc computations
    bool is_clockwise = false;
    if( motion_mode == CW_ARC ) {
        is_clockwise = true;
    }

    // Append arc
    return this->append_arc(gcode, target, offset,  radius, is_clockwise );
}


float Robot::theta(float x, float y)
{
    float t = atanf(x / fabs(y));
    if (y > 0) {
        return(t);
    } else {
        if (t > 0) {
            return(PI - t);
        } else {
            return(-PI - t);
        }
    }
}

void Robot::select_plane(uint8_t axis_0, uint8_t axis_1, uint8_t axis_2)
{
    this->plane_axis_0 = axis_0;
    this->plane_axis_1 = axis_1;
    this->plane_axis_2 = axis_2;
}

void Robot::clearToolOffset()
{
    this->tool_offset = wcs_t(0, 0, 0);
}

void Robot::setToolOffset(const float offset[3])
{
    this->tool_offset = wcs_t(offset[0], offset[1], offset[2]);
}

float Robot::get_feed_rate() const
{
    // TODO
    // return THEKERNEL->gcode_dispatch->get_modal_command() == 0 ? seek_rate : feed_rate;
    return feed_rate;
}
