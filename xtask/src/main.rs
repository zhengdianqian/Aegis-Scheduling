mod zip_ext;

use std::{
    fs,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use fs_extra::{dir, file};
use xshell::{cmd, Shell};
use zip::{write::FileOptions, CompressionMethod};

use crate::zip_ext::zip_create_from_directory_with_options;

#[derive(Parser)]
#[command(name = "xtask", about = "Yumi Build System")]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// 编译 Yumi 项目并打包
    #[command(alias = "b")]
    Build,
}

/// 对外版本号的唯一真值是 module/module.prop 的 version=（如 4.6）。
/// Cargo.toml 的 version 只是合法 semver（4.6.0），不参与产物命名，
/// 避免同一个版本出现 4.6 / 4.6.0 两种写法。
fn module_version() -> Result<String> {
    let content = fs::read_to_string("module/module.prop")
        .context("读取 module/module.prop 失败，无法确定版本号")?;
    for line in content.lines() {
        if let Some(value) = line.trim().strip_prefix("version=") {
            let value = value.trim();
            if !value.is_empty() {
                return Ok(value.to_string());
            }
        }
    }
    anyhow::bail!("module/module.prop 缺少 version=")
}

fn main() -> Result<()> {
    let cli = Cli::parse();
    // 初始化 xshell
    let sh = Shell::new()?;

    match cli.command {
        Commands::Build => build(&sh)?,
    }

    Ok(())
}

fn cal_git_code(sh: &Shell) -> Result<usize> {
    // xshell 极大地简化了获取命令 stdout 的过程
    let output = cmd!(sh, "git rev-list --count HEAD").read()?;
    Ok(output.trim().parse::<usize>()?)
}

fn get_date() -> String {
    chrono::Local::now().format("%Y%m%d-%H%M").to_string()
}

fn build(sh: &Shell) -> Result<()> {
    let temp_dir = temp_dir();

    // 版本号取自 module/module.prop（与 CI、update.json、README 同一个真值）
    let version = module_version()?;

    // 1. 清理并重建临时目录
    let _ = fs::remove_dir_all(&temp_dir);
    fs::create_dir_all(&temp_dir)?;

    // 2. 编译 WebUI
    build_webui(sh)?;

    // 3. 编译 Rust 核心
    build_core(sh)?;

    // 4. 拷贝 module 目录内容
    let module_dir = Path::new("module").to_path_buf();
    dir::copy(
        &module_dir,
        &temp_dir,
        &dir::CopyOptions::new().overwrite(true).content_only(true),
    )?;

    if temp_dir.join(".gitignore").exists() {
        fs::remove_file(temp_dir.join(".gitignore"))?;
    }

    // 5. 组装 bin 目录
    let bin_path = temp_dir.join("core").join("bin");
    fs::create_dir_all(&bin_path)?;
    
    file::copy(
        aarch64_bin_path(),
        bin_path.join("Aegis"),
        &file::CopyOptions::new().overwrite(true),
    )?;
    
    let webroot_dir = temp_dir.join("webroot");
    dir::copy(
        Path::new("webui").join("dist"),
        &webroot_dir,
        &dir::CopyOptions::new().overwrite(true).content_only(true),
    )?;
    
    // 6. 打包 Zip
    let output_dir = Path::new("output");
    fs::create_dir_all(output_dir)?; // 确保 output 目录存在
    
    let zip_filename = format!(
        "aegis-{}-{}-{}.zip",
        version,
        cal_git_code(sh)?,
        get_date()
    );
    let zip_path = output_dir.join(zip_filename);

    println!("开始打包: {}", zip_path.display());

    let options: FileOptions<'_, ()> = FileOptions::default()
        .compression_method(CompressionMethod::Deflated)
        .compression_level(Some(9));
        
    zip_create_from_directory_with_options(&zip_path, &temp_dir, |_| options)?;

    println!("构建并打包成功！");
    Ok(())
}

fn temp_dir() -> PathBuf {
    Path::new("output").join(".temp")
}

fn aarch64_bin_path() -> PathBuf {
    // 尊重 CARGO_TARGET_DIR：仓库路径含非 ASCII 字符时，MinGW dlltool 无法
    // 打开 target 里的临时 .def 文件，需要把构建输出指到纯 ASCII 目录。
    let target_dir = std::env::var_os("CARGO_TARGET_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("target"));
    target_dir
        .join("aarch64-linux-android")
        .join("release")
        .join("Aegis")
}

fn build_core(sh: &Shell) -> Result<()> {
    println!("正在编译 Rust Core...");
    // push_env 会在当前作用域内设置环境变量，离开作用域自动恢复
    let _env = sh.push_env("RUSTFLAGS", "-C default-linker-libraries");
    cmd!(sh, "cargo +nightly ndk --platform 26 -t arm64-v8a build -Z build-std -r --locked --future-incompat-report").run()?;
    Ok(())
}

fn build_webui(sh: &Shell) -> Result<()> {
    println!("正在编译 WebUI...");
    // push_dir 类似于 cd，离开作用域后会自动切回原目录
    let _dir = sh.push_dir("webui");
    // Windows 上 npm 是 npm.cmd，xshell 无法按 PATHEXT 解析，需要走 cmd /c
    #[cfg(windows)]
    cmd!(sh, "cmd /c npm run build").run()?;
    #[cfg(not(windows))]
    cmd!(sh, "npm run build").run()?;
    Ok(())
}
