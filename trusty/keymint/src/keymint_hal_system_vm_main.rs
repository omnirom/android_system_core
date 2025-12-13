//
// Copyright (C) 2025 The Android Open-Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! This module implements the HAL service for Keymint (Rust) interacting with
//! the Trusty VM.

use android_trusty_commservice::aidl::android::trusty::commservice::ICommService::ICommService;
use anyhow::{anyhow, bail, Context, Result};
use binder::{self, AccessorProvider, ProcessState, Strong};
use kmr_hal::{register_binder_services, send_hal_info, SerializedChannel, ALL_HALS};
use log::{error, info, warn};
use std::{
    ops::DerefMut,
    panic,
    sync::{Arc, Mutex},
};

const SERVICE_INSTANCE: &str = "default";

const ACCESSOR_SERVICE_NAME: &str = "android.os.IAccessor/ICommService/security_vm_keymint";
const INTERNAL_RPC_SERVICE_NAME: &str =
    "android.trusty.commservice.ICommService/security_vm_keymint";

#[derive(Debug)]
struct CommServiceChannel {
    comm_service: Strong<dyn ICommService>,
}

impl SerializedChannel for CommServiceChannel {
    const MAX_SIZE: usize = 4000;
    fn execute(&mut self, serialized_req: &[u8]) -> binder::Result<Vec<u8>> {
        self.comm_service.execute_transact(serialized_req)
    }
}

/// Helper struct to provide convenient access to the locked channel.
struct HalChannel(Arc<Mutex<CommServiceChannel>>);

impl HalChannel {
    /// Executes a closure with a mutable reference to the inner channel.
    fn with<F, R>(&self, f: F) -> Result<R>
    where
        F: FnOnce(&mut CommServiceChannel) -> Result<R>,
    {
        let mut channel = self.0.lock().map_err(|_| anyhow!("Mutex was poisoned"))?;
        f(channel.deref_mut())
    }
}

impl From<CommServiceChannel> for HalChannel {
    fn from(channel: CommServiceChannel) -> Self {
        Self(Arc::new(Mutex::new(channel)))
    }
}

fn main() {
    if let Err(e) = inner_main() {
        panic!("HAL service failed: {e:?}");
    }
}

fn inner_main() -> Result<()> {
    setup_logging_and_panic_hook();

    if cfg!(feature = "nonsecure") {
        warn!("Non-secure Trusty KM HAL service is starting.");
    } else {
        info!("Trusty KM HAL service is starting.");
    }

    info!("Starting thread pool.");
    ProcessState::start_thread_pool();

    // TODO(b/429217397): Use a proper way to register an accessor and get the internal RPC
    // service via accessor here.
    let _accessor_provider = AccessorProvider::new(&[INTERNAL_RPC_SERVICE_NAME.to_owned()], |s| {
        binder::wait_for_service(ACCESSOR_SERVICE_NAME)
            .and_then(|service| binder::Accessor::from_binder(s, service))
    })
    .ok_or(anyhow!("failed to create accessor provider"))?;
    let comm_service = binder::wait_for_interface(INTERNAL_RPC_SERVICE_NAME)
        .context("failed to get ICommService interface from accessor")?;
    let channel: HalChannel = CommServiceChannel { comm_service }.into();

    #[cfg(feature = "nonsecure")]
    kmr_hal_nonsecure::send_boot_info_and_attestation_id_info(&channel.0)?;

    register_binder_services(&channel.0, ALL_HALS, SERVICE_INSTANCE)?;

    // Send the HAL service information to the TA
    channel.with(|c| send_hal_info(c).context("failed to populate HAL info"))?;

    info!("Successfully registered KeyMint HAL services. Joining thread pool now.");

    ProcessState::join_thread_pool();
    bail!("Binder thread pool exited unexpectedly, terminating HAL service.");
}

fn setup_logging_and_panic_hook() {
    android_logger::init_once(
        android_logger::Config::default()
            .with_tag("keymint-hal-trusty-vm")
            .with_max_level(log::LevelFilter::Info)
            .with_log_buffer(android_logger::LogId::System),
    );
    // In case of a panic, log it before the process terminates.
    panic::set_hook(Box::new(|panic_info| {
        error!("PANIC: {panic_info}");
    }));
}
