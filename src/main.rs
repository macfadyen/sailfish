use std::fmt::Write;
use std::str::FromStr;
use error::Error::*;
use setup::Setup;
use solver::cpu;
#[cfg(feature = "cuda")]
use solver::gpu;
use solver::omp;
use solver::Solve;

pub mod cmdline;
pub mod error;
pub mod setup;
pub mod solver;
pub mod state;

fn time_exec<F>(mut f: F) -> std::time::Duration
where
    F: FnMut(),
{
    let start = std::time::Instant::now();
    f();
    start.elapsed()
}

fn run() -> Result<(), error::Error> {
    let cmdline = cmdline::parse_command_line()?;

    let (setup_name, parameters) = cmdline.setup.as_ref().map_or((None, None), |s| {
        let mut a = s.as_str().splitn(2, ":");
        let n = a.next();
        let p = a.next();
        (n, p)
    });

    let setup: Box<dyn Setup> = match setup_name {
        Some("binary") => {
            Box::new(setup::Binary::from_str(parameters.unwrap_or(""))?)
        }
        Some("explosion") => {
            Box::new(setup::Explosion::from_str(parameters.unwrap_or(""))?)
        }
        Some(_) | None => {
            let mut message = String::new();
            writeln!(message, "specify setup:").unwrap();
            writeln!(message, "    binary").unwrap();
            writeln!(message, "    explosion").unwrap();
            return Err(PrintUserInformation(message))
        }
    };

    let mesh = setup.mesh(cmdline.resolution);
    let nu = setup.viscosity().unwrap_or(0.0);
    let eos = setup.equation_of_state();
    let buffer = setup.buffer_zone();
    let v_max = setup.max_signal_speed().unwrap();
    let cfl = cmdline.cfl_number;
    let fold = cmdline.fold;
    let rk_order = cmdline.rk_order;
    let checkpoint_interval = cmdline.checkpoint_interval;
    let outdir = &cmdline.outdir;
    let dt = f64::min(mesh.dx, mesh.dy) / v_max * cfl;
    let total_num_zones = mesh.num_total_zones();

    let mut checkpoint = state::RecurringTask { number: 0, next_time: 0.0 };

    let primitive = setup.initial_primitive_vec(&mesh);
    let mut solver: Box<dyn Solve> = match (cmdline.use_omp, cmdline.use_gpu) {
        (false, false) => Box::new(cpu::Solver::new(mesh, primitive)),
        (true, false) => Box::new(omp::Solver::new(mesh, primitive)),
        (false, true) => {
            #[cfg(feature = "cuda")]
            {
                Box::new(gpu::Solver::new(mesh, primitive))
            }
            #[cfg(not(feature = "cuda"))]
            {
                panic!("cuda feature not enabled")
            }
        }
        (true, true) => {
            panic!("omp and gpu cannot be enabled at once")
        }
    };

    let mut time = 0.0;
    let mut iteration = 0;
    let mut mzps_log = vec![];

    while time < cmdline.end_time {
        if time >= checkpoint.next_time {
            checkpoint.next(checkpoint_interval);
            let state = state::State {
                iteration,
                time,
                primitive: solver.primitive(),
                checkpoint: checkpoint.clone(),
                setup: cmdline.setup.clone().unwrap(),
            };
            state::write_checkpoint(&state, outdir, checkpoint.number - 1)?;
        }

        let elapsed = time_exec(|| {
            for _ in 0..fold {
                solver::advance(&mut solver, &eos, &buffer, |t| setup.masses(t), nu, rk_order, time, dt);
                time += dt;
                iteration += 1;
            }
        });

        mzps_log.push((total_num_zones * fold) as f64 / 1e6 / elapsed.as_secs_f64());
        println!(
            "[{}] t={:.3} Mzps={:.3}",
            iteration,
            time,
            mzps_log.last().unwrap()
        );
    }

    checkpoint.next(checkpoint_interval);
    let state = state::State {
        iteration,
        time,
        primitive: solver.primitive(),
        checkpoint: checkpoint.clone(),
        setup: cmdline.setup.clone().unwrap(),
    };
    state::write_checkpoint(&state, outdir, checkpoint.number - 1)
}

fn main() {
    match run() {
        Ok(_) => {}
        Err(e) => print!("{}", e),
    }
}
