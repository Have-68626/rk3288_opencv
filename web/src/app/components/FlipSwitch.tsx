import React from 'react';
import { Switch, Typography } from 'antd';

const { Text } = Typography;

interface FlipSwitchProps {
  label: React.ReactNode;
  checked?: boolean;
  disabled?: boolean;
  onChange?: (checked: boolean) => void;
  checkedChildren?: React.ReactNode;
  unCheckedChildren?: React.ReactNode;
}

const FlipSwitch: React.FC<FlipSwitchProps> = ({
  label,
  checked,
  disabled,
  onChange,
  checkedChildren,
  unCheckedChildren
}) => {
  const id = React.useId();

  const handleToggle = (e: React.MouseEvent) => {
    e.preventDefault(); // Prevent native label click from focusing but not toggling
    if (!disabled && onChange) {
      onChange(!checked);
    }
  };

  return (
    <label
      htmlFor={id}
      onClick={handleToggle}
      style={{
        cursor: disabled ? 'not-allowed' : 'pointer',
        display: 'inline-flex',
        alignItems: 'center',
        gap: 8,
      }}
    >
      {label && <Text disabled={disabled}>{label}</Text>}
      <Switch
        id={id}
        checked={checked}
        disabled={disabled}
        onChange={onChange}
        onClick={(_, e) => e.stopPropagation()}
        checkedChildren={checkedChildren}
        unCheckedChildren={unCheckedChildren}
        aria-label={typeof label === 'string' ? label : undefined}
      />
    </label>
  );
};

export default FlipSwitch;
