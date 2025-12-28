ExUnit.start()

defmodule PmetricsTest.TestHelpers do
  alias PmetricsTest.Repo

  def clear_metrics do
    Repo.query!("SELECT pmetrics.clear_metrics()")
    :ok
  end

  def query(sql, params \\ []) do
    Repo.query!(sql, params)
  end

  def setup_extensions do
    Repo.query!("CREATE EXTENSION IF NOT EXISTS pmetrics")
    Repo.query!("CREATE EXTENSION IF NOT EXISTS pmetrics_stmts")
  end
end
