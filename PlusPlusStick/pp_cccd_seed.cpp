// ESP32 port of pp_cccd_seed.
//
// The nRF firmware forced the peer's CCCD values server-side through
// sd_ble_gatts_sys_attr_set so it could notify on characteristics the app had
// not subscribed to yet. NimBLE lets us notify a specific connection handle
// regardless of subscription, so seeding only has to flip the compat layer's
// "treat as enabled" flag on the same set of characteristics.

#include <Arduino.h>
#include <string.h>

#include "pp_cccd_seed.h"

void ppSeedUserServiceCccds(const CccdSeedRefs& refs, const char* reason)
{
  if (*refs.activeConnHandle == BLE_CONN_HANDLE_INVALID || *refs.userCccdsSeeded) {
    return;
  }

  ppCompatSeedNotifyEnabled(refs.serviceAChar5);
  ppCompatSeedNotifyEnabled(refs.serviceBChar1);
  ppCompatSeedNotifyEnabled(refs.serviceBChar2);
  ppCompatSeedNotifyEnabled(refs.serviceCChar1);
  ppCompatSeedNotifyEnabled(refs.serviceDChar2);
  ppCompatSeedNotifyEnabled(refs.buttonNotifyChar);
  ppCompatSeedNotifyEnabled(refs.authControl);
  ppCompatSeedNotifyEnabled(refs.authBlob);

  *refs.userCccdsSeeded = true;
  // The original read Service D's CCCD back after seeding it to 0x0001.
  *refs.serviceDNotifyEnabled = refs.serviceDChar2->notifyEnabled();

  Serial.print("Seed late full CCCDs via ");
  Serial.print(reason);
  Serial.print(" (compat) serviceD=");
  Serial.println(*refs.serviceDNotifyEnabled ? "1" : "0");
}
