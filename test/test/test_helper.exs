ExUnit.start()

defmodule PmetricsTest.TestHelpers do
  import Ecto.Query
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

  # Get single metric value
  def get_metric_value(name, type, labels \\ %{}) do
    from(m in fragment("pmetrics.list_metrics()"),
      where: m.name == ^name and m.type == ^type and m.labels == ^labels,
      select: m.value
    )
    |> Repo.one()
  end

  # Count metrics by name and type
  def count_metrics(name, type) do
    from(m in fragment("pmetrics.list_metrics()"),
      where: m.name == ^name and m.type == ^type,
      select: count()
    )
    |> Repo.one()
  end

  # List metrics with optional ordering
  def list_metrics(name, type, order_by \\ :bucket) do
    from(m in fragment("pmetrics.list_metrics()"),
      where: m.name == ^name and m.type == ^type,
      order_by: ^order_by,
      select: %{
        name: m.name,
        labels: m.labels,
        type: m.type,
        bucket: m.bucket,
        value: m.value
      }
    )
    |> Repo.all()
  end

  # Get histogram sum
  def get_histogram_sum(name, labels \\ %{}) do
    get_metric_value(name, "histogram_sum", labels)
  end
end
