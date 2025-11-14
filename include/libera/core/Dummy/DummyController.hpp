#pragma once
#include "libera/core/LaserDevice.hpp"


namespace libera::core::dummy {

class DummyController : public LaserDevice {
public:
    DummyController();
    ~DummyController();

   

protected:
    virtual void run() override; // the worker loop

};

} // namespace libera::core::dummy
