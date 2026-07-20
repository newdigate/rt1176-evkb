cmake_minimum_required(VERSION 3.24)
project(cm4_spi_dma_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
evkb_library_dir(SPI EVKB_SPI_DIR)


# CM4 image; consumes the shared LPSPI core (polled begin) from newdigate/SPI.
teensy_add_cm4_image(cm4_spi_dma
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
            ${EVKB_SPI_DIR}/lpspi1176.c
    INCLUDE_DIRS ${EVKB_SPI_DIR})

teensy_add_executable(cm4_spi_dma_test cm4_spi_dma_test.cpp)
teensy_target_link_libraries(cm4_spi_dma_test cores)
target_link_libraries(cm4_spi_dma_test.elf stdc++)
teensy_target_link_cm4_image(cm4_spi_dma_test cm4_spi_dma)
