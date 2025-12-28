import Config

config :pmetrics_test, PmetricsTest.Repo,
  database: System.get_env("PGDATABASE", "demo"),
  username: System.get_env("PGUSER", "postgres"),
  password: System.get_env("PGPASSWORD", "postgres"),
  hostname: System.get_env("PGHOST", "localhost"),
  port: String.to_integer(System.get_env("PGPORT", "5432")),
  pool: Ecto.Adapters.SQL.Sandbox,
  pool_size: 200

config :pmetrics_test, ecto_repos: [PmetricsTest.Repo]

config :logger, level: :error
