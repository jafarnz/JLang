# program.s is produced by: jlang build app.jlang -o main/program.s
idf_component_register(SRCS "main.c" "program.s"
                       INCLUDE_DIRS "../..")
