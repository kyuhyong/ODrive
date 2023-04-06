/*
* @brief Contains board specific variables and initialization functions
*/

#include <board.h>

#include <odrive_main.h>
#include <low_level.h>

#include <Drivers/STM32/stm32_timer.hpp>

#include <adc.h>
#include <dma.h>
#include <tim.h>
#include <usart.h>
#include <freertos_vars.h>

// this should technically be in task_timer.cpp but let's not make a one-line file
bool TaskTimer::enabled = false;

extern "C" void SystemClock_Config(void); // defined in main.c generated by CubeMX

#define ControlLoop_IRQHandler OTG_HS_IRQHandler
#define ControlLoop_IRQn OTG_HS_IRQn

// This array is placed at the very start of the ram (0x20000000) and will be
// used during manufacturing to test the struct that will go to the OTP before
// _actually_ putting anything into OTP. This avoids bulk-destroying STM32's if
// we introduce unintended breakage in our manufacturing scripts.
uint8_t __attribute__((section(".testdata"))) fake_otp[FLASH_OTP_END + 1 - FLASH_OTP_BASE] = {0, 0, 0, HW_VERSION_MAJOR, HW_VERSION_MINOR, HW_VERSION_VOLTAGE};

Stm32SpiArbiter spi3_arbiter{&hspi3};
Stm32SpiArbiter& ext_spi_arbiter = spi3_arbiter;

UART_HandleTypeDef* uart_a = &huart4;
UART_HandleTypeDef* uart_b = &huart2; // TODO: this could be supported in ODrive v3.6 (or similar) using STM32's USART2
UART_HandleTypeDef* uart_c = nullptr;

Drv8301 m0_gate_driver{
    &spi3_arbiter,
    {M0_nCS_GPIO_Port, M0_nCS_Pin}, // nCS
    {}, // EN pin (shared between both motors, therefore we actuate it outside of the drv8301 driver)
    {nFAULT_GPIO_Port, nFAULT_Pin} // nFAULT pin (shared between both motors)
};

Drv8301 m1_gate_driver{
    &spi3_arbiter,
    {M1_nCS_GPIO_Port, M1_nCS_Pin}, // nCS
    {}, // EN pin (shared between both motors, therefore we actuate it outside of the drv8301 driver)
    {nFAULT_GPIO_Port, nFAULT_Pin} // nFAULT pin (shared between both motors)
};

const float fet_thermistor_poly_coeffs[] =
    {363.93910201f, -462.15369634f, 307.55129571f, -27.72569531f};
const size_t fet_thermistor_num_coeffs = sizeof(fet_thermistor_poly_coeffs)/sizeof(fet_thermistor_poly_coeffs[1]);

OnboardThermistorCurrentLimiter fet_thermistors[AXIS_COUNT] = {
    {
        15, // adc_channel
        &fet_thermistor_poly_coeffs[0], // coefficients
        fet_thermistor_num_coeffs // num_coeffs
    }, {
#if HW_VERSION_MAJOR == 3 && HW_VERSION_MINOR >= 3
        4, // adc_channel
#else
        1, // adc_channel
#endif
        &fet_thermistor_poly_coeffs[0], // coefficients
        fet_thermistor_num_coeffs // num_coeffs
    }
};

OffboardThermistorCurrentLimiter motor_thermistors[AXIS_COUNT];

Motor motors[AXIS_COUNT] = {
    {
        &htim1, // timer
        0b110, // current_sensor_mask
        1.0f / SHUNT_RESISTANCE, // shunt_conductance [S]
        m0_gate_driver, // gate_driver
        m0_gate_driver, // opamp
        fet_thermistors[0],
        motor_thermistors[0]
    },
    {
        &htim8, // timer
        0b110, // current_sensor_mask
        1.0f / SHUNT_RESISTANCE, // shunt_conductance [S]
        m1_gate_driver, // gate_driver
        m1_gate_driver, // opamp
        fet_thermistors[1],
        motor_thermistors[1]
    }
};

Encoder encoders[AXIS_COUNT] = {
    {
        &htim3, // timer
        {M0_ENC_Z_GPIO_Port, M0_ENC_Z_Pin}, // index_gpio
        {M0_ENC_A_GPIO_Port, M0_ENC_A_Pin}, // hallA_gpio
        {M0_ENC_B_GPIO_Port, M0_ENC_B_Pin}, // hallB_gpio
        {M0_ENC_Z_GPIO_Port, M0_ENC_Z_Pin}, // hallC_gpio
        &spi3_arbiter // spi_arbiter
    },
    {
        &htim4, // timer
        {M1_ENC_Z_GPIO_Port, M1_ENC_Z_Pin}, // index_gpio
        {M1_ENC_A_GPIO_Port, M1_ENC_A_Pin}, // hallA_gpio
        {M1_ENC_B_GPIO_Port, M1_ENC_B_Pin}, // hallB_gpio
        {M1_ENC_Z_GPIO_Port, M1_ENC_Z_Pin}, // hallC_gpio
        &spi3_arbiter // spi_arbiter
    }
};

// TODO: this has no hardware dependency and should be allocated depending on config
Endstop endstops[2 * AXIS_COUNT];
MechanicalBrake mechanical_brakes[AXIS_COUNT];

SensorlessEstimator sensorless_estimators[AXIS_COUNT];
Controller controllers[AXIS_COUNT];
TrapezoidalTrajectory trap[AXIS_COUNT];

std::array<Axis, AXIS_COUNT> axes{{
    {
        0, // axis_num
        1, // step_gpio_pin
        2, // dir_gpio_pin
        (osPriority)(osPriorityHigh + (osPriority)1), // thread_priority
        encoders[0], // encoder
        sensorless_estimators[0], // sensorless_estimator
        controllers[0], // controller
        motors[0], // motor
        trap[0], // trap
        endstops[0], endstops[1], // min_endstop, max_endstop
        mechanical_brakes[0], // mechanical brake
    },
    {
        1, // axis_num
#if HW_VERSION_MAJOR == 3 && HW_VERSION_MINOR >= 5
        7, // step_gpio_pin
        8, // dir_gpio_pin
#else
        3, // step_gpio_pin
        4, // dir_gpio_pin
#endif
        osPriorityHigh, // thread_priority
        encoders[1], // encoder
        sensorless_estimators[1], // sensorless_estimator
        controllers[1], // controller
        motors[1], // motor
        trap[1], // trap
        endstops[2], endstops[3], // min_endstop, max_endstop
        mechanical_brakes[1], // mechanical brake
    },
}};



#if (HW_VERSION_MINOR == 1) || (HW_VERSION_MINOR == 2)
Stm32Gpio gpios[] = {
    {nullptr, 0}, // dummy GPIO0 so that PCB labels and software numbers match

    {GPIOB, GPIO_PIN_2}, // GPIO1
    {GPIOA, GPIO_PIN_5}, // GPIO2
    {GPIOA, GPIO_PIN_4}, // GPIO3
    {GPIOA, GPIO_PIN_3}, // GPIO4
    {nullptr, 0}, // GPIO5 (doesn't exist on this board)
    {nullptr, 0}, // GPIO6 (doesn't exist on this board)
    {nullptr, 0}, // GPIO7 (doesn't exist on this board)
    {nullptr, 0}, // GPIO8 (doesn't exist on this board)

    {GPIOB, GPIO_PIN_4}, // ENC0_A
    {GPIOB, GPIO_PIN_5}, // ENC0_B
    {GPIOA, GPIO_PIN_15}, // ENC0_Z
    {GPIOB, GPIO_PIN_6}, // ENC1_A
    {GPIOB, GPIO_PIN_7}, // ENC1_B
    {GPIOB, GPIO_PIN_3}, // ENC1_Z
    {GPIOB, GPIO_PIN_8}, // CAN_R
    {GPIOB, GPIO_PIN_9}, // CAN_D
};
#elif (HW_VERSION_MINOR == 3) || (HW_VERSION_MINOR == 4)
Stm32Gpio gpios[] = {
    {nullptr, 0}, // dummy GPIO0 so that PCB labels and software numbers match

    {GPIOA, GPIO_PIN_0}, // GPIO1
    {GPIOA, GPIO_PIN_1}, // GPIO2
    {GPIOA, GPIO_PIN_2}, // GPIO3
    {GPIOA, GPIO_PIN_3}, // GPIO4
    {GPIOB, GPIO_PIN_2}, // GPIO5
    {nullptr, 0}, // GPIO6 (doesn't exist on this board)
    {nullptr, 0}, // GPIO7 (doesn't exist on this board)
    {nullptr, 0}, // GPIO8 (doesn't exist on this board)

    {GPIOB, GPIO_PIN_4}, // ENC0_A
    {GPIOB, GPIO_PIN_5}, // ENC0_B
    {GPIOA, GPIO_PIN_15}, // ENC0_Z
    {GPIOB, GPIO_PIN_6}, // ENC1_A
    {GPIOB, GPIO_PIN_7}, // ENC1_B
    {GPIOB, GPIO_PIN_3}, // ENC1_Z
    {GPIOB, GPIO_PIN_8}, // CAN_R
    {GPIOB, GPIO_PIN_9}, // CAN_D
};
#elif (HW_VERSION_MINOR == 5) || (HW_VERSION_MINOR == 6)
Stm32Gpio gpios[GPIO_COUNT] = {
    {nullptr, 0}, // dummy GPIO0 so that PCB labels and software numbers match

    {GPIOA, GPIO_PIN_0}, // GPIO1
    {GPIOA, GPIO_PIN_1}, // GPIO2
    {GPIOA, GPIO_PIN_2}, // GPIO3
    {GPIOA, GPIO_PIN_3}, // GPIO4
    {GPIOC, GPIO_PIN_4}, // GPIO5
    {GPIOB, GPIO_PIN_2}, // GPIO6
    {GPIOA, GPIO_PIN_15}, // GPIO7
    {GPIOB, GPIO_PIN_3}, // GPIO8
    
    {GPIOB, GPIO_PIN_4}, // ENC0_A
    {GPIOB, GPIO_PIN_5}, // ENC0_B
    {GPIOC, GPIO_PIN_9}, // ENC0_Z
    {GPIOB, GPIO_PIN_6}, // ENC1_A
    {GPIOB, GPIO_PIN_7}, // ENC1_B
    {GPIOC, GPIO_PIN_15}, // ENC1_Z
    {GPIOB, GPIO_PIN_8}, // CAN_R
    {GPIOB, GPIO_PIN_9}, // CAN_D
};
#else
#error "unknown GPIOs"
#endif

std::array<GpioFunction, 3> alternate_functions[GPIO_COUNT] = {
    /* GPIO0 (inexistent): */ {{}},

#if HW_VERSION_MINOR >= 3
    /* GPIO1: */ {{{ODrive::GPIO_MODE_UART_A, GPIO_AF8_UART4}, {ODrive::GPIO_MODE_PWM, GPIO_AF2_TIM5}}},
    /* GPIO2: */ {{{ODrive::GPIO_MODE_UART_A, GPIO_AF8_UART4}, {ODrive::GPIO_MODE_PWM, GPIO_AF2_TIM5}}},
    /* GPIO3: */ {{{ODrive::GPIO_MODE_UART_B, GPIO_AF7_USART2}, {ODrive::GPIO_MODE_PWM, GPIO_AF2_TIM5}}},
#else
    /* GPIO1: */ {{}},
    /* GPIO2: */ {{}},
    /* GPIO3: */ {{}},
#endif

    /* GPIO4: */ {{{ODrive::GPIO_MODE_UART_B, GPIO_AF7_USART2}, {ODrive::GPIO_MODE_PWM, GPIO_AF2_TIM5}}},
    /* GPIO5: */ {{}},
    /* GPIO6: */ {{}},
    /* GPIO7: */ {{}},
    /* GPIO8: */ {{}},
    /* ENC0_A: */ {{{ODrive::GPIO_MODE_ENC0, GPIO_AF2_TIM3}}},
    /* ENC0_B: */ {{{ODrive::GPIO_MODE_ENC0, GPIO_AF2_TIM3}}},
    /* ENC0_Z: */ {{}},
    /* ENC1_A: */ {{{ODrive::GPIO_MODE_I2C_A, GPIO_AF4_I2C1}, {ODrive::GPIO_MODE_ENC1, GPIO_AF2_TIM4}}},
    /* ENC1_B: */ {{{ODrive::GPIO_MODE_I2C_A, GPIO_AF4_I2C1}, {ODrive::GPIO_MODE_ENC1, GPIO_AF2_TIM4}}},
    /* ENC1_Z: */ {{}},
    /* CAN_R: */ {{{ODrive::GPIO_MODE_CAN_A, GPIO_AF9_CAN1}, {ODrive::GPIO_MODE_I2C_A, GPIO_AF4_I2C1}}},
    /* CAN_D: */ {{{ODrive::GPIO_MODE_CAN_A, GPIO_AF9_CAN1}, {ODrive::GPIO_MODE_I2C_A, GPIO_AF4_I2C1}}},
};

#if HW_VERSION_MINOR <= 2
PwmInput pwm0_input{&htim5, {0, 0, 0, 4}}; // 0 means not in use
#else
PwmInput pwm0_input{&htim5, {1, 2, 3, 4}};
#endif

extern USBD_HandleTypeDef hUsbDeviceFS;
USBD_HandleTypeDef& usb_dev_handle = hUsbDeviceFS;

bool check_board_version(const uint8_t* otp_ptr) {
    return (otp_ptr[3] == HW_VERSION_MAJOR) &&
           (otp_ptr[4] == HW_VERSION_MINOR) &&
           (otp_ptr[5] == HW_VERSION_VOLTAGE);
}

void system_init() {
    // Reset of all peripherals, Initializes the Flash interface and the Systick.
    HAL_Init();

    // Configure the system clock
    SystemClock_Config();

    // If the OTP is pristine, use the fake-otp in RAM instead
    /*
    const uint8_t* otp_ptr = (const uint8_t*)FLASH_OTP_BASE;
    if (*otp_ptr == 0xff) {
        otp_ptr = fake_otp;
    }
    */
    const uint8_t* otp_ptr = fake_otp;  // Override OTP check

    // Ensure that the board version for which this firmware is compiled matches
    // the board we're running on.
    if (!check_board_version(otp_ptr)) {
        for (;;);
    }
}

bool board_init() {
    // Initialize all configured peripherals
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_ADC1_Init();
    MX_ADC2_Init();
    MX_TIM1_Init();
    MX_TIM8_Init();
    MX_TIM3_Init();
    MX_TIM4_Init();
    MX_SPI3_Init();
    MX_ADC3_Init();
    MX_TIM2_Init();
    MX_TIM5_Init();
    MX_TIM13_Init();

    // External interrupt lines are individually enabled in stm32_gpio.cpp
    HAL_NVIC_SetPriority(EXTI0_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);
    HAL_NVIC_SetPriority(EXTI1_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI1_IRQn);
    HAL_NVIC_SetPriority(EXTI2_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI2_IRQn);
    HAL_NVIC_SetPriority(EXTI3_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI3_IRQn);
    HAL_NVIC_SetPriority(EXTI4_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI4_IRQn);
    HAL_NVIC_SetPriority(EXTI9_5_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI9_5_IRQn);
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

    HAL_NVIC_SetPriority(ControlLoop_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(ControlLoop_IRQn);

    HAL_NVIC_SetPriority(TIM8_UP_TIM13_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(TIM8_UP_TIM13_IRQn);

    if (odrv.config_.enable_uart_a) {
        uart_a->Init.BaudRate = odrv.config_.uart_a_baudrate;
        MX_UART4_Init();
    }

    if (odrv.config_.enable_uart_b) {
        uart_b->Init.BaudRate = odrv.config_.uart_b_baudrate;
        MX_USART2_UART_Init();
    }

    if (odrv.config_.enable_i2c_a) {
        // Set up the direction GPIO as input
        get_gpio(3).config(GPIO_MODE_INPUT, GPIO_PULLUP);
        get_gpio(4).config(GPIO_MODE_INPUT, GPIO_PULLUP);
        get_gpio(5).config(GPIO_MODE_INPUT, GPIO_PULLUP);

        osDelay(1); // This has no effect but was here before.
        i2c_stats_.addr = (0xD << 3);
        i2c_stats_.addr |= get_gpio(3).read() ? 0x1 : 0;
        i2c_stats_.addr |= get_gpio(4).read() ? 0x2 : 0;
        i2c_stats_.addr |= get_gpio(5).read() ? 0x4 : 0;
        MX_I2C1_Init(i2c_stats_.addr);
    }

    if (odrv.config_.enable_can_a) {
        // The CAN initialization will (and must) init its own GPIOs before the
        // GPIO modes are initialized. Therefore we ensure that the later GPIO
        // mode initialization won't override the CAN mode.
        if (odrv.config_.gpio_modes[15] != ODriveIntf::GPIO_MODE_CAN_A || odrv.config_.gpio_modes[16] != ODriveIntf::GPIO_MODE_CAN_A) {
            odrv.misconfigured_ = true;
        }
    }

    // Ensure that debug halting of the core doesn't leave the motor PWM running
    __HAL_DBGMCU_FREEZE_TIM1();
    __HAL_DBGMCU_FREEZE_TIM8();
    __HAL_DBGMCU_FREEZE_TIM13();

    Stm32Gpio drv_enable_gpio = {EN_GATE_GPIO_Port, EN_GATE_Pin};

    // Reset both DRV chips. The enable pin also controls the SPI interface, not
    // only the driver stages.
    drv_enable_gpio.write(false);
    delay_us(40); // mimumum pull-down time for full reset: 20us
    drv_enable_gpio.write(true);
    delay_us(20000); // mimumum pull-down time for full reset: 20us

    return true;
}

void start_timers() {
    CRITICAL_SECTION() {
        // Temporarily disable ADC triggers so they don't trigger as a side
        // effect of starting the timers.
        hadc1.Instance->CR2 &= ~(ADC_CR2_JEXTEN);
        hadc2.Instance->CR2 &= ~(ADC_CR2_EXTEN | ADC_CR2_JEXTEN);
        hadc3.Instance->CR2 &= ~(ADC_CR2_EXTEN | ADC_CR2_JEXTEN);

        /*
        * Synchronize TIM1, TIM8 and TIM13 such that:
        *  1. The triangle waveform of TIM1 leads the triangle waveform of TIM8 by a
        *     90° phase shift.
        *  2. Each TIM13 reload coincides with a TIM1 lower update event.
        */
        Stm32Timer::start_synchronously<3>(
            {&htim1, &htim8, &htim13},
            {TIM1_INIT_COUNT, 0, TIM1_INIT_COUNT / 2 /* TIM13 is on a clock that's only have as fast as TIM1 */}
        );

        hadc1.Instance->CR2 |= (ADC_EXTERNALTRIGINJECCONVEDGE_RISING);
        hadc2.Instance->CR2 |= (ADC_EXTERNALTRIGCONVEDGE_RISING | ADC_EXTERNALTRIGINJECCONVEDGE_RISING);
        hadc3.Instance->CR2 |= (ADC_EXTERNALTRIGCONVEDGE_RISING | ADC_EXTERNALTRIGINJECCONVEDGE_RISING);

        __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_JEOC);
        __HAL_ADC_CLEAR_FLAG(&hadc2, ADC_FLAG_JEOC);
        __HAL_ADC_CLEAR_FLAG(&hadc3, ADC_FLAG_JEOC);
        __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_EOC);
        __HAL_ADC_CLEAR_FLAG(&hadc2, ADC_FLAG_EOC);
        __HAL_ADC_CLEAR_FLAG(&hadc3, ADC_FLAG_EOC);
        __HAL_ADC_CLEAR_FLAG(&hadc1, ADC_FLAG_OVR);
        __HAL_ADC_CLEAR_FLAG(&hadc2, ADC_FLAG_OVR);
        __HAL_ADC_CLEAR_FLAG(&hadc3, ADC_FLAG_OVR);
        
        __HAL_TIM_CLEAR_IT(&htim8, TIM_IT_UPDATE);
        __HAL_TIM_ENABLE_IT(&htim8, TIM_IT_UPDATE);
    }
}

static bool fetch_and_reset_adcs(
        std::optional<Iph_ABC_t>* current0,
        std::optional<Iph_ABC_t>* current1) {
    bool all_adcs_done = (ADC1->SR & ADC_SR_JEOC) == ADC_SR_JEOC
        && (ADC2->SR & (ADC_SR_EOC | ADC_SR_JEOC)) == (ADC_SR_EOC | ADC_SR_JEOC)
        && (ADC3->SR & (ADC_SR_EOC | ADC_SR_JEOC)) == (ADC_SR_EOC | ADC_SR_JEOC);
    if (!all_adcs_done) {
        return false;
    }

    vbus_sense_adc_cb(ADC1->JDR1);

    if (m0_gate_driver.is_ready()) {
        std::optional<float> phB = motors[0].phase_current_from_adcval(ADC2->JDR1);
        std::optional<float> phC = motors[0].phase_current_from_adcval(ADC3->JDR1);
        if (phB.has_value() && phC.has_value()) {
            *current0 = {-*phB - *phC, *phB, *phC};
        }
    }

    if (m1_gate_driver.is_ready()) {
        std::optional<float> phB = motors[1].phase_current_from_adcval(ADC2->DR);
        std::optional<float> phC = motors[1].phase_current_from_adcval(ADC3->DR);
        if (phB.has_value() && phC.has_value()) {
            *current1 = {-*phB - *phC, *phB, *phC};
        }
    }
    
    ADC1->SR = ~(ADC_SR_JEOC);
    ADC2->SR = ~(ADC_SR_EOC | ADC_SR_JEOC | ADC_SR_OVR);
    ADC3->SR = ~(ADC_SR_EOC | ADC_SR_JEOC | ADC_SR_OVR);

    return true;
}

extern "C" {

void HAL_SPI_TxCpltCallback(SPI_HandleTypeDef *hspi) {
    HAL_SPI_TxRxCpltCallback(hspi);
}

void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi) {
    HAL_SPI_TxRxCpltCallback(hspi);
}

void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi) {
    if (hspi == &hspi3) {
        spi3_arbiter.on_complete();
    }
}

void TIM5_IRQHandler(void) {
    COUNT_IRQ(TIM5_IRQn);
    pwm0_input.on_capture();
}

volatile uint32_t timestamp_ = 0;
volatile bool counting_down_ = false;

void TIM8_UP_TIM13_IRQHandler(void) {
    COUNT_IRQ(TIM8_UP_TIM13_IRQn);
    
    // Entry into this function happens at 21-23 clock cycles after the timer
    // update event.
    __HAL_TIM_CLEAR_IT(&htim8, TIM_IT_UPDATE);

    // If the corresponding timer is counting up, we just sampled in SVM vector 0, i.e. real current
    // If we are counting down, we just sampled in SVM vector 7, with zero current
    bool counting_down = TIM8->CR1 & TIM_CR1_DIR;

    bool timer_update_missed = (counting_down_ == counting_down);
    if (timer_update_missed) {
        motors[0].disarm_with_error(Motor::ERROR_TIMER_UPDATE_MISSED);
        motors[1].disarm_with_error(Motor::ERROR_TIMER_UPDATE_MISSED);
        return;
    }
    counting_down_ = counting_down;

    timestamp_ += TIM_1_8_PERIOD_CLOCKS * (TIM_1_8_RCR + 1);

    if (!counting_down) {
        TaskTimer::enabled = odrv.task_timers_armed_;
        // Run sampling handlers and kick off control tasks when TIM8 is
        // counting up.
        odrv.sampling_cb();
        NVIC->STIR = ControlLoop_IRQn;
    } else {
        // Tentatively reset all PWM outputs to 50% duty cycles. If the control
        // loop handler finishes in time then these values will be overridden
        // before they go into effect.
        TIM1->CCR1 =
        TIM1->CCR2 =
        TIM1->CCR3 =
        TIM8->CCR1 =
        TIM8->CCR2 =
        TIM8->CCR3 =
            TIM_1_8_PERIOD_CLOCKS / 2;
    }
}

void ControlLoop_IRQHandler(void) {
    COUNT_IRQ(ControlLoop_IRQn);
    uint32_t timestamp = timestamp_;

    // Ensure that all the ADCs are done
    std::optional<Iph_ABC_t> current0;
    std::optional<Iph_ABC_t> current1;

    if (!fetch_and_reset_adcs(&current0, &current1)) {
        motors[0].disarm_with_error(Motor::ERROR_BAD_TIMING);
        motors[1].disarm_with_error(Motor::ERROR_BAD_TIMING);
    }

    // If the motor FETs are not switching then we can't measure the current
    // because for this we need the low side FET to conduct.
    // So for now we guess the current to be 0 (this is not correct shortly after
    // disarming and when the motor spins fast in idle). Passing an invalid
    // current reading would create problems with starting FOC.
    if (!(TIM1->BDTR & TIM_BDTR_MOE_Msk)) {
        current0 = {0.0f, 0.0f};
    }
    if (!(TIM8->BDTR & TIM_BDTR_MOE_Msk)) {
        current1 = {0.0f, 0.0f};
    }

    motors[0].current_meas_cb(timestamp - TIM1_INIT_COUNT, current0);
    motors[1].current_meas_cb(timestamp, current1);

    odrv.control_loop_cb(timestamp);

    // By this time the ADCs for both M0 and M1 should have fired again. But
    // let's wait for them just to be sure.
    MEASURE_TIME(odrv.task_times_.dc_calib_wait) {
        while (!(ADC2->SR & ADC_SR_EOC));
    }

    if (!fetch_and_reset_adcs(&current0, &current1)) {
        motors[0].disarm_with_error(Motor::ERROR_BAD_TIMING);
        motors[1].disarm_with_error(Motor::ERROR_BAD_TIMING);
    }

    motors[0].dc_calib_cb(timestamp + TIM_1_8_PERIOD_CLOCKS * (TIM_1_8_RCR + 1) - TIM1_INIT_COUNT, current0);
    motors[1].dc_calib_cb(timestamp + TIM_1_8_PERIOD_CLOCKS * (TIM_1_8_RCR + 1), current1);

    motors[0].pwm_update_cb(timestamp + 3 * TIM_1_8_PERIOD_CLOCKS * (TIM_1_8_RCR + 1) - TIM1_INIT_COUNT);
    motors[1].pwm_update_cb(timestamp + 3 * TIM_1_8_PERIOD_CLOCKS * (TIM_1_8_RCR + 1));

    // If we did everything right, the TIM8 update handler should have been
    // called exactly once between the start of this function and now.

    if (timestamp_ != timestamp + TIM_1_8_PERIOD_CLOCKS * (TIM_1_8_RCR + 1)) {
        motors[0].disarm_with_error(Motor::ERROR_CONTROL_DEADLINE_MISSED);
        motors[1].disarm_with_error(Motor::ERROR_CONTROL_DEADLINE_MISSED);
    }

    odrv.task_timers_armed_ = odrv.task_timers_armed_ && !TaskTimer::enabled;
    TaskTimer::enabled = false;
}

void I2C1_EV_IRQHandler(void) {
    COUNT_IRQ(I2C1_EV_IRQn);
    HAL_I2C_EV_IRQHandler(&hi2c1);
}

void I2C1_ER_IRQHandler(void) {
    COUNT_IRQ(I2C1_ER_IRQn);
    HAL_I2C_ER_IRQHandler(&hi2c1);
}

extern PCD_HandleTypeDef hpcd_USB_OTG_FS; // defined in usbd_conf.c
void OTG_FS_IRQHandler(void) {
    COUNT_IRQ(OTG_FS_IRQn);
    HAL_PCD_IRQHandler(&hpcd_USB_OTG_FS);
}

}
