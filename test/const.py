#!/usr/bin/env python
#
#title           :const.py
#description     :
#
# const class does not allow rebinding value to name so that name is constant.
#
#==========================================================================================

class _const:
    class ConstError(TypeError): pass
    def __setattr__(self,name,value):
        if name in self.__dict__:
            message = "Can't rebind const " + name
            raise self.ConstError(message)
        self.__dict__[name]=value
import sys
sys.modules[__name__]=_const()
