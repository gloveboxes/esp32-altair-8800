idf_component_register(SRCS "main.c"
                             "../Altair8800/intel8080.c"
                             "../Altair8800/memory.c"
                             "../Altair8800/pico_88dcdd_flash.c"
                       INCLUDE_DIRS "." 
                                    "../Altair8800"
                                    "../Disks")
