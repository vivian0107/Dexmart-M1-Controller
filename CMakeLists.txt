cmake_minimum_required(VERSION 3.20)
project(m1_diagnostic_firmware LANGUAGES C ASM)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(M1_MOTION_APPLICATION "Build the separately commissioned M1 motion firmware" OFF)
if(M1_MOTION_APPLICATION)
    set(M1_RUNTIME Core/Src/m1_motion.c Core/Src/m1_motion_board.c)
else()
    set(M1_RUNTIME Core/Src/m1_diagnostics.c Core/Src/m1_diag_board.c)
endif()
set(HAL_DIR Drivers/STM32G4xx_HAL_Driver)
set(HAL_NAMES hal hal_cortex hal_dma hal_dma_ex hal_exti hal_fdcan hal_flash
    hal_flash_ex hal_flash_ramfunc hal_gpio hal_pwr hal_pwr_ex hal_rcc hal_rcc_ex
    hal_uart hal_uart_ex)
set(HAL_SOURCES)
foreach(name IN LISTS HAL_NAMES)
    list(APPEND HAL_SOURCES "${HAL_DIR}/Src/stm32g4xx_${name}.c")
endforeach()
add_executable(m1_diagnostics.elf
    Core/Src/main.c Core/Src/stm32g4xx_it.c Core/Src/stm32g4xx_hal_msp.c
    Core/Src/system_stm32g4xx.c Core/Src/m1_canopen.c ${M1_RUNTIME}
    Core/Src/m1_gcc_runtime.c
    Drivers/BSP/STM32G4xx_Nucleo/stm32g4xx_nucleo.c
    Drivers/CMSIS/Device/ST/STM32G4xx/Source/Templates/gcc/startup_stm32g474xx.s
    ${HAL_SOURCES})
target_include_directories(m1_diagnostics.elf PRIVATE Core/Inc ${HAL_DIR}/Inc ${HAL_DIR}/Inc/Legacy
    Drivers/BSP/STM32G4xx_Nucleo Drivers/CMSIS/Include Drivers/CMSIS/Device/ST/STM32G4xx/Include)
target_compile_definitions(m1_diagnostics.elf PRIVATE STM32G474xx USE_HAL_DRIVER USE_NUCLEO_64)
set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
target_compile_options(m1_diagnostics.elf PRIVATE ${CPU_FLAGS} -Os -g3 -ffunction-sections -fdata-sections
    "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror>")
target_link_options(m1_diagnostics.elf PRIVATE ${CPU_FLAGS} -nostartfiles --specs=nano.specs --specs=nosys.specs
    "-T${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32g474re.ld" -Wl,--gc-sections
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/m1_diagnostics.map" -Wl,--print-memory-usage)
target_link_libraries(m1_diagnostics.elf PRIVATE m)
if(M1_MOTION_APPLICATION)
    target_compile_definitions(m1_diagnostics.elf PRIVATE M1_MOTION_APPLICATION)
    target_link_options(m1_diagnostics.elf PRIVATE -u _scanf_float)
    set_target_properties(m1_diagnostics.elf PROPERTIES OUTPUT_NAME m1_motion.elf)
    set(M1_OUTPUT m1_motion)
else()
    set(M1_OUTPUT m1_diagnostics)
endif()
set_property(TARGET m1_diagnostics.elf APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stm32g474re.ld")
add_custom_command(TARGET m1_diagnostics.elf POST_BUILD
    COMMAND ${ARM_OBJCOPY} -O ihex $<TARGET_FILE:m1_diagnostics.elf> ${CMAKE_CURRENT_BINARY_DIR}/${M1_OUTPUT}.hex
    COMMAND ${ARM_OBJCOPY} -O binary $<TARGET_FILE:m1_diagnostics.elf> ${CMAKE_CURRENT_BINARY_DIR}/${M1_OUTPUT}.bin
    COMMAND ${ARM_SIZE} $<TARGET_FILE:m1_diagnostics.elf>)
