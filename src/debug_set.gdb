# debug_set.gdb
file ./redis-server

# 设置关键断点
break setCommand
break createStringObject
break dbAdd
break dictAdd

# 设置条件断点，只在处理SET命令时触发
break processCommand if (strcmp(c->argv[0]->ptr, "set") == 0)

# 运行配置
set pagination off
run